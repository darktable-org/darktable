/*
    This file is part of darktable,
    Copyright (C) 2009-2021 darktable developers.

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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/opencl.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/rgb_norms.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/paint.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "gui/accelerators.h"
#include "iop/iop_api.h"
#include "libs/colorpicker.h"

#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(1)

#define DT_IOP_TONECURVE_RES 256
#define DT_IOP_TONECURVE_MAXNODES 20

DT_MODULE_INTROSPECTION(5, dt_iop_tonecurve_params_t)

static gboolean dt_iop_tonecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data);
static gboolean dt_iop_tonecurve_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
static gboolean dt_iop_tonecurve_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_iop_tonecurve_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
static gboolean dt_iop_tonecurve_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
static gboolean dt_iop_tonecurve_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);


typedef enum tonecurve_channel_t
{
  ch_L = 0,
  ch_a = 1,
  ch_b = 2,
  ch_max = 3
} tonecurve_channel_t;
/*
typedef enum dt_iop_tonecurve_preservecolors_t
{
  DT_TONECURVE_PRESERVE_NONE = 0,
  DT_TONECURVE_PRESERVE_LUMINANCE = 1,
  DT_TONECURVE_PRESERVE_LMAX = 2,
  DT_TONECURVE_PRESERVE_LAVG = 3,
  DT_TONECURVE_PRESERVE_LSUM = 4,
  DT_TONECURVE_PRESERVE_LNORM = 5,
  DT_TONECURVE_PRESERVE_LBP = 6,
} dt_iop_tonecurve_preservecolors_t;
*/
typedef struct dt_iop_tonecurve_node_t
{
  float x;
  float y;
} dt_iop_tonecurve_node_t;

typedef enum dt_iop_tonecurve_autoscale_t
{
  DT_S_SCALE_AUTOMATIC = 1,        /* automatically adjust saturation based on L_out/L_in
                                      $DESCRIPTION: "Lab, linked channels" */
  DT_S_SCALE_MANUAL = 0,           /* user specified curves
                                      $DESCRIPTION: "Lab, independent channels" */
  DT_S_SCALE_AUTOMATIC_XYZ = 2,    /* automatically adjust saturation by
                                      transforming the curve C to C' like:
                                      L_out=C(L_in) -> Y_out=C'(Y_in) and applying C' to the X and Z
                                      channels, too (and then transforming it back to Lab of course)
                                      $DESCRIPTION: "XYZ, linked channels" */
  DT_S_SCALE_AUTOMATIC_RGB = 3,    /* similar to above but use an rgb working space
                                      $DESCRIPTION: "RGB, linked channels" */
} dt_iop_tonecurve_autoscale_t;

// parameter structure of tonecurve 1st version, needed for use in legacy_params()
typedef struct dt_iop_tonecurve_params1_t
{
  float tonecurve_x[6], tonecurve_y[6];
  int tonecurve_preset;
} dt_iop_tonecurve_params1_t;

// parameter structure of tonecurve 3rd version, needed for use in legacy_params()
typedef struct dt_iop_tonecurve_params3_t
{
  dt_iop_tonecurve_node_t tonecurve[3][DT_IOP_TONECURVE_MAXNODES]; // three curves (L, a, b) with max number
                                                                   // of nodes
  int tonecurve_nodes[3];
  int tonecurve_type[3];
  int tonecurve_autoscale_ab;
  int tonecurve_preset;
} dt_iop_tonecurve_params3_t;

typedef struct dt_iop_tonecurve_params4_t
{
  dt_iop_tonecurve_node_t tonecurve[3][DT_IOP_TONECURVE_MAXNODES]; // three curves (L, a, b) with max number
                                                                   // of nodes
  int tonecurve_nodes[3];
  int tonecurve_type[3];
  int tonecurve_autoscale_ab;
  int tonecurve_preset;
  int tonecurve_unbound_ab;
} dt_iop_tonecurve_params4_t;

typedef struct dt_iop_tonecurve_params_t
{
  dt_iop_tonecurve_node_t tonecurve[3][DT_IOP_TONECURVE_MAXNODES]; // three curves (L, a, b) with max number
                                                                   // of nodes
  int tonecurve_nodes[3];
  int tonecurve_type[3]; // $DEFAULT: MONOTONE_HERMITE
  dt_iop_tonecurve_autoscale_t tonecurve_autoscale_ab; //$DEFAULT: DT_S_SCALE_AUTOMATIC_RGB $DESCRIPTION: "color space"
  int tonecurve_preset; // $DEFAULT: 0
  int tonecurve_unbound_ab; // $DEFAULT: 1
  dt_iop_rgb_norms_t preserve_colors; // $DEFAULT: DT_RGB_NORM_AVERAGE $DESCRIPTION: "preserve colors"
} dt_iop_tonecurve_params_t;

typedef struct dt_iop_tonecurve_gui_data_t
{
  dt_draw_curve_t *minmax_curve[3]; // curves for gui to draw
  int minmax_curve_nodes[3];
  int minmax_curve_type[3];
  GtkBox *hbox;
  GtkDrawingArea *area;
  GtkSizeGroup *sizegroup;
  GtkWidget *autoscale_ab;
  GtkNotebook *channel_tabs;
  GtkWidget *colorpicker;
  GtkWidget *interpolator;
  tonecurve_channel_t channel;
  double mouse_x, mouse_y;
  int selected;
  float draw_xs[DT_IOP_TONECURVE_RES], draw_ys[DT_IOP_TONECURVE_RES];
  float draw_min_xs[DT_IOP_TONECURVE_RES], draw_min_ys[DT_IOP_TONECURVE_RES];
  float draw_max_xs[DT_IOP_TONECURVE_RES], draw_max_ys[DT_IOP_TONECURVE_RES];
  float loglogscale;
  int semilog;
  GtkWidget *logbase;
  GtkWidget *preserve_colors;
} dt_iop_tonecurve_gui_data_t;

typedef struct dt_iop_tonecurve_data_t
{
  dt_draw_curve_t *curve[3];     // curves for pipe piece and pixel processing
  int curve_nodes[3];            // number of nodes
  int curve_type[3];             // curve style (e.g. CUBIC_SPLINE)
  float table[3][0x10000];       // precomputed look-up tables for tone curve
  float unbounded_coeffs_L[3];   // approximation for extrapolation of L
  float unbounded_coeffs_ab[12]; // approximation for extrapolation of ab (left and right)
  int autoscale_ab;
  int unbound_ab;
  int preserve_colors;
} dt_iop_tonecurve_data_t;

typedef struct dt_iop_tonecurve_global_data_t
{
  float picked_color[3];
  float picked_color_min[3];
  float picked_color_max[3];
  float picked_output_color[3];
  int kernel_tonecurve;
} dt_iop_tonecurve_global_data_t;


const char *name()
{
  return _("tone curve");
}

int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_GRADING;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("alter an image’s tones using curves"),
                                      _("corrective and creative"),
                                      _("linear or non-linear, Lab, display-referred"),
                                      _("non-linear, Lab"),
                                      _("non-linear, Lab, display-referred"));
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 5)
  {
    dt_iop_tonecurve_params1_t *o = (dt_iop_tonecurve_params1_t *)old_params;
    dt_iop_tonecurve_params_t *n = (dt_iop_tonecurve_params_t *)new_params;

    // start with a fresh copy of default parameters
    // unfortunately default_params aren't inited at this stage.
    *n = (dt_iop_tonecurve_params_t){ { { { 0.0, 0.0 }, { 1.0, 1.0 } },
                                        { { 0.0, 0.0 }, { 0.5, 0.5 }, { 1.0, 1.0 } },
                                        { { 0.0, 0.0 }, { 0.5, 0.5 }, { 1.0, 1.0 } } },
                                      { 2, 3, 3 },
                                      { MONOTONE_HERMITE, MONOTONE_HERMITE, MONOTONE_HERMITE },
                                      1,
                                      0,
                                      1 };
    for(int k = 0; k < 6; k++) n->tonecurve[ch_L][k].x = o->tonecurve_x[k];
    for(int k = 0; k < 6; k++) n->tonecurve[ch_L][k].y = o->tonecurve_y[k];
    n->tonecurve_nodes[ch_L] = 6;
    n->tonecurve_type[ch_L] = CUBIC_SPLINE;
    n->tonecurve_autoscale_ab = 1;
    n->tonecurve_preset = o->tonecurve_preset;
    n->tonecurve_unbound_ab = 0;
    n->preserve_colors = 0;
    return 0;
  }
  else if(old_version == 2 && new_version == 5)
  {
    // version 2 never really materialized so there should be no legacy history stacks of that version around
    return 1;
  }
  else if(old_version == 3 && new_version == 5)
  {
    dt_iop_tonecurve_params3_t *o = (dt_iop_tonecurve_params3_t *)old_params;
    dt_iop_tonecurve_params_t *n = (dt_iop_tonecurve_params_t *)new_params;

    memcpy(n->tonecurve, o->tonecurve, sizeof(n->tonecurve));
    memcpy(n->tonecurve_nodes, o->tonecurve_nodes, sizeof(n->tonecurve_nodes));
    memcpy(n->tonecurve_type, o->tonecurve_type, sizeof(n->tonecurve_type));
    n->tonecurve_autoscale_ab = o->tonecurve_autoscale_ab;
    n->tonecurve_preset = o->tonecurve_preset;
    n->tonecurve_unbound_ab = 0;
    n->preserve_colors = 0;
    return 0;
  }
  else if(old_version == 4 && new_version == 5)
  {
    dt_iop_tonecurve_params4_t *o = (dt_iop_tonecurve_params4_t *)old_params;
    dt_iop_tonecurve_params_t *n = (dt_iop_tonecurve_params_t *)new_params;

    memcpy(n->tonecurve, o->tonecurve, sizeof(dt_iop_tonecurve_params4_t));
    n->preserve_colors = 0;
    return 0;
  }
  return 1;
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)piece->data;
  dt_iop_tonecurve_global_data_t *gd = (dt_iop_tonecurve_global_data_t *)self->global_data;
  cl_mem dev_L = NULL;
  cl_mem dev_a = NULL;
  cl_mem dev_b = NULL;
  cl_mem dev_coeffs_L = NULL;
  cl_mem dev_coeffs_ab = NULL;
  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int autoscale_ab = d->autoscale_ab;
  const int unbound_ab = d->unbound_ab;
  const float low_approximation = d->table[0][(int)(0.01f * 0x10000ul)];
  const int preserve_colors = d->preserve_colors;

  const dt_iop_order_iccprofile_info_t *const work_profile
    = dt_ioppr_add_profile_info_to_list(self->dev, DT_COLORSPACE_PROPHOTO_RGB, "", INTENT_PERCEPTUAL);

  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl;
  cl_float *profile_lut_cl = NULL;

  err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid, &profile_info_cl, &profile_lut_cl,
                                            &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS) goto error;

  dev_L = dt_opencl_copy_host_to_device(devid, d->table[ch_L], 256, 256, sizeof(float));
  if(dev_L == NULL) goto error;

  dev_a = dt_opencl_copy_host_to_device(devid, d->table[ch_a], 256, 256, sizeof(float));
  if(dev_a == NULL) goto error;

  dev_b = dt_opencl_copy_host_to_device(devid, d->table[ch_b], 256, 256, sizeof(float));
  if(dev_b == NULL) goto error;

  dev_coeffs_L = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->unbounded_coeffs_L);
  if(dev_coeffs_L == NULL) goto error;

  dev_coeffs_ab = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 12, d->unbounded_coeffs_ab);
  if(dev_coeffs_ab == NULL) goto error;

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 4, sizeof(cl_mem), (void *)&dev_L);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 5, sizeof(cl_mem), (void *)&dev_a);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 6, sizeof(cl_mem), (void *)&dev_b);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 7, sizeof(int), (void *)&autoscale_ab);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 8, sizeof(int), (void *)&unbound_ab);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 9, sizeof(cl_mem), (void *)&dev_coeffs_L);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 10, sizeof(cl_mem), (void *)&dev_coeffs_ab);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 11, sizeof(float), (void *)&low_approximation);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 12, sizeof(int), (void *)&preserve_colors);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 13, sizeof(cl_mem), (void *)&dev_profile_info);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 14, sizeof(cl_mem), (void *)&dev_profile_lut);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_tonecurve, sizes);

  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(dev_L);
  dt_opencl_release_mem_object(dev_a);
  dt_opencl_release_mem_object(dev_b);
  dt_opencl_release_mem_object(dev_coeffs_L);
  dt_opencl_release_mem_object(dev_coeffs_ab);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_L);
  dt_opencl_release_mem_object(dev_a);
  dt_opencl_release_mem_object(dev_b);
  dt_opencl_release_mem_object(dev_coeffs_L);
  dt_opencl_release_mem_object(dev_coeffs_ab);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  dt_print(DT_DEBUG_OPENCL, "[opencl_tonecurve] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif
/*
static inline float dt_prophoto_rgb_luminance(const float *const rgb)
{
  return (rgb[0] * 0.2880402f + rgb[1] * 0.7118741f + rgb[2] * 0.0000857f);
}
*/
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         i, o, roi_in, roi_out))
    return; // image has been copied through to output and module's trouble flag has been updated

  const dt_iop_tonecurve_data_t *const restrict d = (dt_iop_tonecurve_data_t *)(piece->data);
  const dt_iop_order_iccprofile_info_t *const work_profile
    = dt_ioppr_add_profile_info_to_list(self->dev, DT_COLORSPACE_PROPHOTO_RGB, "", INTENT_PERCEPTUAL);
  const float xm_L = 1.0f / d->unbounded_coeffs_L[0];
  const float xm_ar = 1.0f / d->unbounded_coeffs_ab[0];
  const float xm_al = 1.0f - 1.0f / d->unbounded_coeffs_ab[3];
  const float xm_br = 1.0f / d->unbounded_coeffs_ab[6];
  const float xm_bl = 1.0f - 1.0f / d->unbounded_coeffs_ab[9];
  const float low_approximation = d->table[0][(int)(0.01f * 0x10000ul)];

  const size_t npixels = (size_t)roi_out->width * roi_out->height;
  const int autoscale_ab = d->autoscale_ab;
  const int unbound_ab = d->unbound_ab;

  const float *const restrict in = (float*)i;
  float *const restrict out = (float*)o;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels, autoscale_ab, low_approximation, xm_al, xm_ar, xm_bl, xm_br, xm_L, unbound_ab, work_profile) \
  dt_omp_sharedconst(d, in, out) \
  schedule(static)
#endif
  for(int k = 0; k < 4*npixels; k += 4)
  {
    const float L_in = in[k] / 100.0f;

    out[k+0] = (L_in < xm_L) ? d->table[ch_L][CLAMP((int)(L_in * 0x10000ul), 0, 0xffff)]
      : dt_iop_eval_exp(d->unbounded_coeffs_L, L_in);

    if(autoscale_ab == DT_S_SCALE_MANUAL)
    {
      const float a_in = (in[k+1] + 128.0f) / 256.0f;
      const float b_in = (in[k+2] + 128.0f) / 256.0f;

      if(unbound_ab == 0)
      {
        // old style handling of a/b curves: only lut lookup with clamping
        out[k+1] = d->table[ch_a][CLAMP((int)(a_in * 0x10000ul), 0, 0xffff)];
        out[k+2] = d->table[ch_b][CLAMP((int)(b_in * 0x10000ul), 0, 0xffff)];
      }
      else
      {
        // new style handling of a/b curves: lut lookup with two-sided extrapolation;
        // mind the x-axis reversal for the left-handed side
        out[k+1] = (a_in > xm_ar)
          ? dt_iop_eval_exp(d->unbounded_coeffs_ab, a_in)
          : ((a_in < xm_al) ? dt_iop_eval_exp(d->unbounded_coeffs_ab + 3, 1.0f - a_in)
             : d->table[ch_a][CLAMP((int)(a_in * 0x10000ul), 0, 0xffff)]);
        out[k+2] = (b_in > xm_br)
          ? dt_iop_eval_exp(d->unbounded_coeffs_ab + 6, b_in)
          : ((b_in < xm_bl) ? dt_iop_eval_exp(d->unbounded_coeffs_ab + 9, 1.0f - b_in)
             : d->table[ch_b][CLAMP((int)(b_in * 0x10000ul), 0, 0xffff)]);
      }
    }
    else if(autoscale_ab == DT_S_SCALE_AUTOMATIC)
    {
      // in Lab: correct compressed Luminance for saturation:
      if(L_in > 0.01f)
      {
        out[k+1] = in[k+1] * out[k] / in[k+0];
        out[k+2] = in[k+2] * out[k] / in[k+0];
      }
      else
      {
        out[k+1] = in[k+1] * low_approximation;
        out[k+2] = in[k+2] * low_approximation;
      }
    }
    else if(autoscale_ab == DT_S_SCALE_AUTOMATIC_XYZ)
    {
      dt_aligned_pixel_t XYZ;
      dt_Lab_to_XYZ(in + k, XYZ);
      for(int c=0;c<3;c++)
        XYZ[c] = (XYZ[c] < xm_L) ? d->table[ch_L][CLAMP((int)(XYZ[c] * 0x10000ul), 0, 0xffff)]
          : dt_iop_eval_exp(d->unbounded_coeffs_L, XYZ[c]);
      dt_XYZ_to_Lab(XYZ, out + k);
    }
    else if(autoscale_ab == DT_S_SCALE_AUTOMATIC_RGB)
    {
      dt_aligned_pixel_t rgb = {0, 0, 0};
      dt_Lab_to_prophotorgb(in + k, rgb);
      if(d->preserve_colors == DT_RGB_NORM_NONE)
      {
        for(int c = 0; c < 3; c++)
        {
          rgb[c] = (rgb[c] < xm_L) ? d->table[ch_L][CLAMP((int)(rgb[c] * 0x10000ul), 0, 0xffff)]
            : dt_iop_eval_exp(d->unbounded_coeffs_L, rgb[c]);
        }
      }
      else
      {
        float ratio = 1.f;
        const float lum = dt_rgb_norm(rgb, d->preserve_colors, work_profile);
        if(lum > 0.f)
        {
          const float curve_lum = (lum < xm_L)
            ? d->table[ch_L][CLAMP((int)(lum * 0x10000ul), 0, 0xffff)]
            : dt_iop_eval_exp(d->unbounded_coeffs_L, lum);
          ratio = curve_lum / lum;
        }
        for(size_t c = 0; c < 3; c++)
        {
          rgb[c] = (ratio * rgb[c]);
        }
      }
      dt_prophotorgb_to_Lab(rgb, out + k);
    }

    out[k+3] = in[k+3];
  }
}

static const struct
{
  const char *name;
  const char *maker;
  const char *model;
  int iso_min;
  float iso_max;
  struct dt_iop_tonecurve_params_t preset;
} preset_camera_curves[] = {
  // This is where you can paste the line provided by dt-curve-tool
  // Here is a valid example for you to compare
  // clang-format off
    // nikon d750 by Edouard Gomez
    {"Nikon D750", "NIKON CORPORATION", "NIKON D750", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.083508, 0.073677}, {0.212191, 0.274799}, {0.397095, 0.594035}, {0.495025, 0.714660}, {0.683565, 0.878550}, {0.854059, 0.950927}, {1.000000, 1.000000}}, {{0.000000, 0.000000}, {0.125000, 0.125000}, {0.250000, 0.250000}, {0.375000, 0.375000}, {0.500000, 0.500000}, {0.625000, 0.625000}, {0.750000, 0.750000}, {0.875000, 0.875000}}, {{0.000000, 0.000000}, {0.125000, 0.125000}, {0.250000, 0.250000}, {0.375000, 0.375000}, {0.500000, 0.500000}, {0.625000, 0.625000}, {0.750000, 0.750000}, {0.875000, 0.875000} }}, {8, 8, 8}, {2, 2, 2}, 1, 0, 0}},
    // nikon d5100 contributed by Stefan Kauerauf
    {"NIKON D5100", "NIKON CORPORATION", "NIKON D5100", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.000957, 0.000176}, {0.002423, 0.000798}, {0.005893, 0.003685}, {0.013219, 0.006619}, {0.023372, 0.011954}, {0.037580, 0.017817}, {0.069695, 0.035353}, {0.077276, 0.040315}, {0.123707, 0.082707}, {0.145249, 0.112105}, {0.189168, 0.186135}, {0.219576, 0.243677}, {0.290201, 0.385251}, {0.428150, 0.613355}, {0.506199, 0.700256}, {0.622833, 0.805488}, {0.702763, 0.870959}, {0.935053, 0.990139}, {1.000000, 1.000000}}, {{0.000000, 0.000000}, {0.050000, 0.050000}, {0.100000, 0.100000}, {0.150000, 0.150000}, {0.200000, 0.200000}, {0.250000, 0.250000}, {0.300000, 0.300000}, {0.350000, 0.350000}, {0.400000, 0.400000}, {0.450000, 0.450000}, {0.500000, 0.500000}, {0.550000, 0.550000}, {0.600000, 0.600000}, {0.650000, 0.650000}, {0.700000, 0.700000}, {0.750000, 0.750000}, {0.800000, 0.800000}, {0.850000, 0.850000}, {0.900000, 0.900000}, {0.950000, 0.950000}}, {{0.000000, 0.000000}, {0.050000, 0.050000}, {0.100000, 0.100000}, {0.150000, 0.150000}, {0.200000, 0.200000}, {0.250000, 0.250000}, {0.300000, 0.300000}, {0.350000, 0.350000}, {0.400000, 0.400000}, {0.450000, 0.450000}, {0.500000, 0.500000}, {0.550000, 0.550000}, {0.600000, 0.600000}, {0.650000, 0.650000}, {0.700000, 0.700000}, {0.750000, 0.750000}, {0.800000, 0.800000}, {0.850000, 0.850000}, {0.900000, 0.900000}, {0.950000, 0.950000}}}, {20, 20, 20}, {2, 2, 2}, 1, 0, 0}},
    // nikon d7000 by Edouard Gomez
    {"Nikon D7000", "NIKON CORPORATION", "NIKON D7000", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.110633, 0.111192}, {0.209771, 0.286963}, {0.355888, 0.561236}, {0.454987, 0.673098}, {0.769212, 0.920485}, {0.800468, 0.933428}, {1.000000, 1.000000}}, {{0.000000, 0.000000}, {0.125000, 0.125000}, {0.250000, 0.250000}, {0.375000, 0.375000}, {0.500000, 0.500000}, {0.625000, 0.625000}, {0.750000, 0.750000}, {0.875000, 0.875000}}, {{0.000000, 0.000000}, {0.125000, 0.125000}, {0.250000, 0.250000}, {0.375000, 0.375000}, {0.500000, 0.500000}, {0.625000, 0.625000}, {0.750000, 0.750000}, {0.875000, 0.875000}}}, {8, 8, 8}, {2, 2, 2}, 1, 0, 0}},
    // nikon d7200 standard by Ralf Brown (firmware 1.00)
    {"Nikon D7200", "NIKON CORPORATION", "NIKON D7200", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.000618, 0.003286}, {0.001639, 0.003705}, {0.005227, 0.005101}, {0.013299, 0.011192}, {0.016048, 0.013130}, {0.037941, 0.027014}, {0.058195, 0.041339}, {0.086531, 0.069088}, {0.116679, 0.107283}, {0.155629, 0.159422}, {0.205477, 0.246265}, {0.225923, 0.287343}, {0.348056, 0.509104}, {0.360629, 0.534732}, {0.507562, 0.762089}, {0.606899, 0.865692}, {0.734828, 0.947468}, {0.895488, 0.992021}, {1.000000, 1.000000}}, {{0.000000, 0.000000}, {0.050000, 0.050000}, {0.100000, 0.100000}, {0.150000, 0.150000}, {0.200000, 0.200000}, {0.250000, 0.250000}, {0.300000, 0.300000}, {0.350000, 0.350000}, {0.400000, 0.400000}, {0.450000, 0.450000}, {0.500000, 0.500000}, {0.550000, 0.550000}, {0.600000, 0.600000}, {0.650000, 0.650000}, {0.700000, 0.700000}, {0.750000, 0.750000}, {0.800000, 0.800000}, {0.850000, 0.850000}, {0.900000, 0.900000}, {0.950000, 0.950000}}, {{0.000000, 0.000000}, {0.050000, 0.050000}, {0.100000, 0.100000}, {0.150000, 0.150000}, {0.200000, 0.200000}, {0.250000, 0.250000}, {0.300000, 0.300000}, {0.350000, 0.350000}, {0.400000, 0.400000}, {0.450000, 0.450000}, {0.500000, 0.500000}, {0.550000, 0.550000}, {0.600000, 0.600000}, {0.650000, 0.650000}, {0.700000, 0.700000}, {0.750000, 0.750000}, {0.800000, 0.800000}, {0.850000, 0.850000}, {0.900000, 0.900000}, {0.950000, 0.950000}}}, {20, 20, 20}, {2, 2, 2}, 1, 0, 0}},
    // nikon d7500 by Anders Bennehag (firmware C 1.00, LD 2.016)
    {"NIKON D7500", "NIKON CORPORATION", "NIKON D7500", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.000421, 0.003412}, {0.003775, 0.004001}, {0.013762, 0.008704}, {0.016698, 0.010230}, {0.034965, 0.018732}, {0.087311, 0.049808}, {0.101389, 0.060789}, {0.166845, 0.145269}, {0.230944, 0.271288}, {0.333399, 0.502609}, {0.353207, 0.542549}, {0.550014, 0.819535}, {0.731749, 0.944033}, {0.783283, 0.960546}, {1.000000, 1.000000}}, {{0.000000, 0.000000}, {0.062500, 0.062500}, {0.125000, 0.125000}, {0.187500, 0.187500}, {0.250000, 0.250000}, {0.312500, 0.312500}, {0.375000, 0.375000}, {0.437500, 0.437500}, {0.500000, 0.500000}, {0.562500, 0.562500}, {0.625000, 0.625000}, {0.687500, 0.687500}, {0.750000, 0.750000}, {0.812500, 0.812500}, {0.875000, 0.875000}, {0.937500, 0.937500}}, {{0.000000, 0.000000}, {0.062500, 0.062500}, {0.125000, 0.125000}, {0.187500, 0.187500}, {0.250000, 0.250000}, {0.312500, 0.312500}, {0.375000, 0.375000}, {0.437500, 0.437500}, {0.500000, 0.500000}, {0.562500, 0.562500}, {0.625000, 0.625000}, {0.687500, 0.687500}, {0.750000, 0.750000}, {0.812500, 0.812500}, {0.875000, 0.875000}, {0.937500, 0.937500}}}, {16, 16, 16}, {2, 2, 2}, 1, 0, 0}},
    // nikon d90 by Edouard Gomez
    {"Nikon D90", "NIKON CORPORATION", "NIKON D90", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.002915, 0.006453}, {0.023324, 0.021601}, {0.078717, 0.074963}, {0.186589, 0.242230}, {0.364432, 0.544956}, {0.629738, 0.814127}, {1.000000, 1.000000}}, {{0.000000, 0.000000}, {0.125000, 0.125000}, {0.250000, 0.250000}, {0.375000, 0.375000}, {0.500000, 0.500000}, {0.625000, 0.625000}, {0.750000, 0.750000}, {0.875000, 0.875000}}, {{0.000000, 0.000000}, {0.125000, 0.125000}, {0.250000, 0.250000}, {0.375000, 0.375000}, {0.500000, 0.500000}, {0.625000, 0.625000}, {0.750000, 0.750000}, {0.875000, 0.875000}}}, {8, 8, 8}, {2, 2, 2}, 1, 0, 0}},
    // Olympus OM-D E-M10 II by Lukas Schrangl
    {"Olympus OM-D E-M10 II", "OLYMPUS CORPORATION    ", "E-M10MarkII     ", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.004036, 0.000809}, {0.015047, 0.009425}, {0.051948, 0.042053}, {0.071777, 0.066635}, {0.090018, 0.086722}, {0.110197, 0.118773}, {0.145817, 0.171861}, {0.207476, 0.278652}, {0.266832, 0.402823}, {0.428061, 0.696319}, {0.559728, 0.847113}, {0.943576, 0.993482}, {1.000000, 1.000000}}, {{0.000000, 0.000000}, {0.071429, 0.071429}, {0.142857, 0.142857}, {0.214286, 0.214286}, {0.285714, 0.285714}, {0.357143, 0.357143}, {0.428571, 0.428571}, {0.500000, 0.500000}, {0.571429, 0.571429}, {0.642857, 0.642857}, {0.714286, 0.714286}, {0.785714, 0.785714}, {0.857143, 0.857143}, {0.928571, 0.928571}}, {{0.000000, 0.000000}, {0.071429, 0.071429}, {0.142857, 0.142857}, {0.214286, 0.214286}, {0.285714, 0.285714}, {0.357143, 0.357143}, {0.428571, 0.428571}, {0.500000, 0.500000}, {0.571429, 0.571429}, {0.642857, 0.642857}, {0.714286, 0.714286}, {0.785714, 0.785714}, {0.857143, 0.857143}, {0.928571, 0.928571}}}, {14, 14, 14}, {2, 2, 2}, 1, 0, 0}},
  // clang-format on
};

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_tonecurve_params_t p;
  memset(&p, 0, sizeof(p));
  p.tonecurve_nodes[ch_L] = 6;
  p.tonecurve_nodes[ch_a] = 7;
  p.tonecurve_nodes[ch_b] = 7;
  p.tonecurve_type[ch_L] = CUBIC_SPLINE;
  p.tonecurve_type[ch_a] = CUBIC_SPLINE;
  p.tonecurve_type[ch_b] = CUBIC_SPLINE;
  p.tonecurve_preset = 0;
  p.tonecurve_autoscale_ab = DT_S_SCALE_AUTOMATIC_RGB;
  p.tonecurve_unbound_ab = 1;

  float linear_ab[7] = { 0.0, 0.08, 0.3, 0.5, 0.7, 0.92, 1.0 };

  // linear a, b curves for presets
  for(int k = 0; k < 7; k++) p.tonecurve[ch_a][k].x = linear_ab[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_a][k].y = linear_ab[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_b][k].x = linear_ab[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_b][k].y = linear_ab[k];

  // More useful low contrast curve (based on Samsung NX -2 Contrast)
  p.tonecurve[ch_L][0].x = 0.000000;
  p.tonecurve[ch_L][1].x = 0.003862;
  p.tonecurve[ch_L][2].x = 0.076613;
  p.tonecurve[ch_L][3].x = 0.169355;
  p.tonecurve[ch_L][4].x = 0.774194;
  p.tonecurve[ch_L][5].x = 1.000000;
  p.tonecurve[ch_L][0].y = 0.000000;
  p.tonecurve[ch_L][1].y = 0.007782;
  p.tonecurve[ch_L][2].y = 0.156182;
  p.tonecurve[ch_L][3].y = 0.290352;
  p.tonecurve[ch_L][4].y = 0.773852;
  p.tonecurve[ch_L][5].y = 1.000000;
  dt_gui_presets_add_generic(_("contrast compression"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  p.tonecurve_nodes[ch_L] = 7;
  float linear_L[7] = { 0.0, 0.08, 0.17, 0.50, 0.83, 0.92, 1.0 };

  // Linear - no contrast
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].y = linear_L[k];
  dt_gui_presets_add_generic(_("gamma 1.0 (linear)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Linear contrast
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].y = linear_L[k];
  p.tonecurve[ch_L][1].y -= 0.020;
  p.tonecurve[ch_L][2].y -= 0.030;
  p.tonecurve[ch_L][4].y += 0.030;
  p.tonecurve[ch_L][5].y += 0.020;
  dt_gui_presets_add_generic(_("contrast - med (linear)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].y = linear_L[k];
  p.tonecurve[ch_L][1].y -= 0.040;
  p.tonecurve[ch_L][2].y -= 0.060;
  p.tonecurve[ch_L][4].y += 0.060;
  p.tonecurve[ch_L][5].y += 0.040;
  dt_gui_presets_add_generic(_("contrast - high (linear)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Gamma contrast
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].y = linear_L[k];
  p.tonecurve[ch_L][1].y -= 0.020;
  p.tonecurve[ch_L][2].y -= 0.030;
  p.tonecurve[ch_L][4].y += 0.030;
  p.tonecurve[ch_L][5].y += 0.020;
  for(int k = 1; k < 6; k++) p.tonecurve[ch_L][k].x = powf(p.tonecurve[ch_L][k].x, 2.2f);
  for(int k = 1; k < 6; k++) p.tonecurve[ch_L][k].y = powf(p.tonecurve[ch_L][k].y, 2.2f);
  dt_gui_presets_add_generic(_("contrast - med (gamma 2.2)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].y = linear_L[k];
  p.tonecurve[ch_L][1].y -= 0.040;
  p.tonecurve[ch_L][2].y -= 0.060;
  p.tonecurve[ch_L][4].y += 0.060;
  p.tonecurve[ch_L][5].y += 0.040;
  for(int k = 1; k < 6; k++) p.tonecurve[ch_L][k].x = powf(p.tonecurve[ch_L][k].x, 2.2f);
  for(int k = 1; k < 6; k++) p.tonecurve[ch_L][k].y = powf(p.tonecurve[ch_L][k].y, 2.2f);
  dt_gui_presets_add_generic(_("contrast - high (gamma 2.2)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  /** for pure power-like functions, we need more nodes close to the bounds**/

  p.tonecurve_type[ch_L] = MONOTONE_HERMITE;

  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].y = linear_L[k];

  // Gamma 2.0 - no contrast
  for(int k = 1; k < 6; k++) p.tonecurve[ch_L][k].y = powf(linear_L[k], 2.0f);
  dt_gui_presets_add_generic(_("gamma 2.0"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Gamma 0.5 - no contrast
  for(int k = 1; k < 6; k++) p.tonecurve[ch_L][k].y = powf(linear_L[k], 0.5f);
  dt_gui_presets_add_generic(_("gamma 0.5"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Log2 - no contrast
  for(int k = 1; k < 6; k++) p.tonecurve[ch_L][k].y = logf(linear_L[k] + 1.0f) / logf(2.0f);
  dt_gui_presets_add_generic(_("logarithm (base 2)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Exp2 - no contrast
  for(int k = 1; k < 6; k++) p.tonecurve[ch_L][k].y = powf(2.0f, linear_L[k]) - 1.0f;
  dt_gui_presets_add_generic(_("exponential (base 2)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  for(int k=0; k<sizeof(preset_camera_curves)/sizeof(preset_camera_curves[0]); k++)
  {
    // insert the preset
    dt_gui_presets_add_generic(preset_camera_curves[k].name, self->op, self->version(),
                               &preset_camera_curves[k].preset, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

    // restrict it to model, maker
    dt_gui_presets_update_mml(preset_camera_curves[k].name, self->op, self->version(),
                              preset_camera_curves[k].maker, preset_camera_curves[k].model, "");

    // restrict it to  iso
    dt_gui_presets_update_iso(preset_camera_curves[k].name, self->op, self->version(),
                              preset_camera_curves[k].iso_min, preset_camera_curves[k].iso_max);

    // restrict it to raw images
    dt_gui_presets_update_ldr(preset_camera_curves[k].name, self->op, self->version(), FOR_RAW);

    // hide all non-matching presets in case the model string is set.
    dt_gui_presets_update_filter(preset_camera_curves[k].name, self->op, self->version(), 1);
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)p1;

  if((pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW)
    piece->request_histogram |= (DT_REQUEST_ON);
  else
    piece->request_histogram &= ~(DT_REQUEST_ON);

  for(int ch = 0; ch < ch_max; ch++)
  {
    // take care of possible change of curve type or number of nodes (not yet implemented in UI)
    if(d->curve_type[ch] != p->tonecurve_type[ch] || d->curve_nodes[ch] != p->tonecurve_nodes[ch])
    {
      dt_draw_curve_destroy(d->curve[ch]);
      d->curve[ch] = dt_draw_curve_new(0.0, 1.0, p->tonecurve_type[ch]);
      d->curve_nodes[ch] = p->tonecurve_nodes[ch];
      d->curve_type[ch] = p->tonecurve_type[ch];
      for(int k = 0; k < p->tonecurve_nodes[ch]; k++)
        (void)dt_draw_curve_add_point(d->curve[ch], p->tonecurve[ch][k].x, p->tonecurve[ch][k].y);
    }
    else
    {
      for(int k = 0; k < p->tonecurve_nodes[ch]; k++)
        dt_draw_curve_set_point(d->curve[ch], k, p->tonecurve[ch][k].x, p->tonecurve[ch][k].y);
    }
    dt_draw_curve_calc_values(d->curve[ch], 0.0f, 1.0f, 0x10000, NULL, d->table[ch]);
  }
  for(int k = 0; k < 0x10000; k++) d->table[ch_L][k] *= 100.0f;
  for(int k = 0; k < 0x10000; k++) d->table[ch_a][k] = d->table[ch_a][k] * 256.0f - 128.0f;
  for(int k = 0; k < 0x10000; k++) d->table[ch_b][k] = d->table[ch_b][k] * 256.0f - 128.0f;

  piece->process_cl_ready = 1;
  if(p->tonecurve_autoscale_ab == DT_S_SCALE_AUTOMATIC_XYZ)
  {
    // derive curve for XYZ:
    for(int k=0;k<0x10000;k++)
    {
      dt_aligned_pixel_t XYZ = {k/(float)0x10000, k/(float)0x10000, k/(float)0x10000};
      dt_aligned_pixel_t Lab = {0.0};
      dt_XYZ_to_Lab(XYZ, Lab);
      Lab[0] = d->table[ch_L][CLAMP((int)(Lab[0]/100.0f * 0x10000), 0, 0xffff)];
      dt_Lab_to_XYZ(Lab, XYZ);
      d->table[ch_L][k] = XYZ[1]; // now mapping Y_in to Y_out
    }
  }
  else if(p->tonecurve_autoscale_ab == DT_S_SCALE_AUTOMATIC_RGB)
  {
    // derive curve for rgb:
    for(int k=0;k<0x10000;k++)
    {
      dt_aligned_pixel_t rgb = {k/(float)0x10000, k/(float)0x10000, k/(float)0x10000};
      dt_aligned_pixel_t Lab = {0.0};
      dt_prophotorgb_to_Lab(rgb, Lab);
      Lab[0] = d->table[ch_L][CLAMP((int)(Lab[0]/100.0f * 0x10000), 0, 0xffff)];
      dt_Lab_to_prophotorgb(Lab, rgb);
      d->table[ch_L][k] = rgb[1]; // now mapping G_in to G_out
    }
  }

  d->autoscale_ab = p->tonecurve_autoscale_ab;
  d->unbound_ab = p->tonecurve_unbound_ab;
  d->preserve_colors = p->preserve_colors;

  // extrapolation for L-curve (right hand side only):
  const float xm_L = p->tonecurve[ch_L][p->tonecurve_nodes[ch_L] - 1].x;
  const float x_L[4] = { 0.7f * xm_L, 0.8f * xm_L, 0.9f * xm_L, 1.0f * xm_L };
  const float y_L[4] = { d->table[ch_L][CLAMP((int)(x_L[0] * 0x10000ul), 0, 0xffff)],
                         d->table[ch_L][CLAMP((int)(x_L[1] * 0x10000ul), 0, 0xffff)],
                         d->table[ch_L][CLAMP((int)(x_L[2] * 0x10000ul), 0, 0xffff)],
                         d->table[ch_L][CLAMP((int)(x_L[3] * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(x_L, y_L, 4, d->unbounded_coeffs_L);

  // extrapolation for a-curve right side:
  const float xm_ar = p->tonecurve[ch_a][p->tonecurve_nodes[ch_a] - 1].x;
  const float x_ar[4] = { 0.7f * xm_ar, 0.8f * xm_ar, 0.9f * xm_ar, 1.0f * xm_ar };
  const float y_ar[4] = { d->table[ch_a][CLAMP((int)(x_ar[0] * 0x10000ul), 0, 0xffff)],
                          d->table[ch_a][CLAMP((int)(x_ar[1] * 0x10000ul), 0, 0xffff)],
                          d->table[ch_a][CLAMP((int)(x_ar[2] * 0x10000ul), 0, 0xffff)],
                          d->table[ch_a][CLAMP((int)(x_ar[3] * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(x_ar, y_ar, 4, d->unbounded_coeffs_ab);

  // extrapolation for a-curve left side (we need to mirror the x-axis):
  const float xm_al = 1.0f - p->tonecurve[ch_a][0].x;
  const float x_al[4] = { 0.7f * xm_al, 0.8f * xm_al, 0.9f * xm_al, 1.0f * xm_al };
  const float y_al[4] = { d->table[ch_a][CLAMP((int)((1.0f - x_al[0]) * 0x10000ul), 0, 0xffff)],
                          d->table[ch_a][CLAMP((int)((1.0f - x_al[1]) * 0x10000ul), 0, 0xffff)],
                          d->table[ch_a][CLAMP((int)((1.0f - x_al[2]) * 0x10000ul), 0, 0xffff)],
                          d->table[ch_a][CLAMP((int)((1.0f - x_al[3]) * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(x_al, y_al, 4, d->unbounded_coeffs_ab + 3);

  // extrapolation for b-curve right side:
  const float xm_br = p->tonecurve[ch_b][p->tonecurve_nodes[ch_b] - 1].x;
  const float x_br[4] = { 0.7f * xm_br, 0.8f * xm_br, 0.9f * xm_br, 1.0f * xm_br };
  const float y_br[4] = { d->table[ch_b][CLAMP((int)(x_br[0] * 0x10000ul), 0, 0xffff)],
                          d->table[ch_b][CLAMP((int)(x_br[1] * 0x10000ul), 0, 0xffff)],
                          d->table[ch_b][CLAMP((int)(x_br[2] * 0x10000ul), 0, 0xffff)],
                          d->table[ch_b][CLAMP((int)(x_br[3] * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(x_br, y_br, 4, d->unbounded_coeffs_ab + 6);

  // extrapolation for b-curve left side (we need to mirror the x-axis):
  const float xm_bl = 1.0f - p->tonecurve[ch_b][0].x;
  const float x_bl[4] = { 0.7f * xm_bl, 0.8f * xm_bl, 0.9f * xm_bl, 1.0f * xm_bl };
  const float y_bl[4] = { d->table[ch_b][CLAMP((int)((1.0f - x_bl[0]) * 0x10000ul), 0, 0xffff)],
                          d->table[ch_b][CLAMP((int)((1.0f - x_bl[1]) * 0x10000ul), 0, 0xffff)],
                          d->table[ch_b][CLAMP((int)((1.0f - x_bl[2]) * 0x10000ul), 0, 0xffff)],
                          d->table[ch_b][CLAMP((int)((1.0f - x_bl[3]) * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(x_bl, y_bl, 4, d->unbounded_coeffs_ab + 9);
}

static float eval_grey(float x)
{
  // "log base" is a combined scaling and offset change so that x->[0,1] with
  // the left side of the histogram expanded (slider->right) or not (slider left, linear)
  return x;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // create part of the pixelpipe
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)malloc(sizeof(dt_iop_tonecurve_data_t));
  dt_iop_tonecurve_params_t *default_params = (dt_iop_tonecurve_params_t *)self->default_params;
  piece->data = (void *)d;
  d->autoscale_ab = DT_S_SCALE_AUTOMATIC;
  d->unbound_ab = 1;
  for(int ch = 0; ch < ch_max; ch++)
  {
    d->curve[ch] = dt_draw_curve_new(0.0, 1.0, default_params->tonecurve_type[ch]);
    d->curve_nodes[ch] = default_params->tonecurve_nodes[ch];
    d->curve_type[ch] = default_params->tonecurve_type[ch];
    for(int k = 0; k < default_params->tonecurve_nodes[ch]; k++)
      (void)dt_draw_curve_add_point(d->curve[ch], default_params->tonecurve[ch][k].x,
                                    default_params->tonecurve[ch][k].y);
  }
  for(int k = 0; k < 0x10000; k++) d->table[ch_L][k] = 100.0f * k / 0x10000;          // identity for L
  for(int k = 0; k < 0x10000; k++) d->table[ch_a][k] = 256.0f * k / 0x10000 - 128.0f; // identity for a
  for(int k = 0; k < 0x10000; k++) d->table[ch_b][k] = 256.0f * k / 0x10000 - 128.0f; // identity for b
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);
  for(int ch = 0; ch < ch_max; ch++) dt_draw_curve_destroy(d->curve[ch]);
  free(piece->data);
  piece->data = NULL;
}

void gui_reset(struct dt_iop_module_t *self)
{
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_bauhaus_combobox_set(g->interpolator, p->tonecurve_type[ch_L]);
  dt_bauhaus_combobox_set(g->preserve_colors, p->preserve_colors);
  dt_bauhaus_slider_set(g->logbase, 0);
  g->loglogscale = 0;
  g->semilog = 0;

  g->channel = (tonecurve_channel_t)ch_L;
  gtk_widget_queue_draw(self->widget);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;

  gui_changed(self, g->autoscale_ab, 0);

  dt_bauhaus_combobox_set(g->interpolator, p->tonecurve_type[ch_L]);
  g->loglogscale = eval_grey(dt_bauhaus_slider_get(g->logbase));
  // that's all, gui curve is read directly from params during expose event.
  gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);

  module->request_histogram |= (DT_REQUEST_ON);

  dt_iop_tonecurve_params_t *d = module->default_params;

  d->tonecurve_nodes[0] = 2;
  d->tonecurve_nodes[1] =
  d->tonecurve_nodes[2] = 3;
  d->tonecurve[0][1].x = d->tonecurve[0][1].y =
  d->tonecurve[1][2].x = d->tonecurve[1][2].y =
  d->tonecurve[2][2].x = d->tonecurve[2][2].y = 1.0f;
  d->tonecurve[1][1].x = d->tonecurve[1][1].y =
  d->tonecurve[2][1].x = d->tonecurve[2][1].y = 0.5f;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_tonecurve_global_data_t *gd
      = (dt_iop_tonecurve_global_data_t *)malloc(sizeof(dt_iop_tonecurve_global_data_t));
  module->data = gd;
  gd->kernel_tonecurve = dt_opencl_create_kernel(program, "tonecurve");
  for(int k=0; k<3; k++)
  {
    gd->picked_color[k] = .0f;
    gd->picked_color_min[k] = .0f;
    gd->picked_color_max[k] = .0f;
    gd->picked_output_color[k] = .0f;
  }
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_tonecurve_global_data_t *gd = (dt_iop_tonecurve_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_tonecurve);
  free(module->data);
  module->data = NULL;
}

static void logbase_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  g->loglogscale = eval_grey(dt_bauhaus_slider_get(g->logbase));
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;

  if(w == g->autoscale_ab)
  {
    g->channel = (tonecurve_channel_t)ch_L;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g->channel_tabs), ch_L);

    gtk_notebook_set_show_tabs(g->channel_tabs, p->tonecurve_autoscale_ab == DT_S_SCALE_MANUAL);
    gtk_widget_set_visible(g->preserve_colors, p->tonecurve_autoscale_ab == DT_S_SCALE_AUTOMATIC_RGB);

    gtk_widget_queue_draw(self->widget);
  }
}

static void interpolator_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  const int combo = dt_bauhaus_combobox_get(widget);
  if(combo == 0) p->tonecurve_type[ch_L] = p->tonecurve_type[ch_a] = p->tonecurve_type[ch_b] = CUBIC_SPLINE;
  if(combo == 1) p->tonecurve_type[ch_L] = p->tonecurve_type[ch_a] = p->tonecurve_type[ch_b] = CATMULL_ROM;
  if(combo == 2) p->tonecurve_type[ch_L] = p->tonecurve_type[ch_a] = p->tonecurve_type[ch_b] = MONOTONE_HERMITE;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

static void tab_switch(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  if(darktable.gui->reset) return;
  c->channel = (tonecurve_channel_t)page_num;
  gtk_widget_queue_draw(self->widget);
}

static gboolean area_resized(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  GtkRequisition r;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  r.width = allocation.width;
  r.height = allocation.width;
  gtk_widget_get_preferred_size(widget, &r, NULL);
  return TRUE;
}

static float to_log(const float x, const float base, const int ch, const int semilog, const int is_ordinate)
{
  // don't log-encode the a and b channels
  if(base > 0.0f && ch == ch_L)
  {
    if(semilog == 1 && is_ordinate == 1)
    {
      // we don't want log on ordinate axis in semilog x
      return x;
    }
    else if(semilog == -1 && is_ordinate == 0)
    {
      // we don't want log on abcissa axis in semilog y
      return x;
    }
    else
    {
      return logf(x * base + 1.0f) / logf(base + 1.0f);
    }
  }
  else
  {
    return x;
  }
}

static float to_lin(const float x, const float base, const int ch, const int semilog, const int is_ordinate)
{
  // don't log-encode the a and b channels
  if(base > 0.0f && ch == ch_L)
  {
    if(semilog == 1 && is_ordinate == 1)
    {
      // we don't want log on ordinate axis in semilog x
      return x;
    }
    else if(semilog == -1 && is_ordinate == 0)
    {
      // we don't want log on abcissa axis in semilog y
      return x;
    }
    else
    {
      return (powf(base + 1.0f, x) - 1.0f) / base;
    }
  }
  else
  {
    return x;
  }
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_tonecurve_global_data_t *gd = (dt_iop_tonecurve_global_data_t *)self->global_data;

  for(int k=0; k<3; k++)
  {
    gd->picked_color[k] = self->picked_color[k];
    gd->picked_color_min[k] = self->picked_color_min[k];
    gd->picked_color_max[k] = self->picked_color_max[k];
    gd->picked_output_color[k] = self->picked_output_color[k];
  }
  dt_control_queue_redraw_widget(self->widget);
}

static void dt_iop_tonecurve_sanity_check(dt_iop_module_t *self, GtkWidget *widget)
{
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;

  int ch = c->channel;
  int nodes = p->tonecurve_nodes[ch];
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[ch];
  int autoscale_ab = p->tonecurve_autoscale_ab;

  // if autoscale_ab is on: do not modify a and b curves
  if((autoscale_ab != DT_S_SCALE_MANUAL) && ch != ch_L) return;

  if(nodes <= 2) return;

  const float mx = tonecurve[c->selected].x;

  // delete vertex if order has changed
  // for all points, x coordinate of point must be strictly larger than
  // the x coordinate of the previous point
  if((c->selected > 0 && (tonecurve[c->selected - 1].x >= mx))
     || (c->selected < nodes - 1 && (tonecurve[c->selected + 1].x <= mx)))
  {
    for(int k = c->selected; k < nodes - 1; k++)
    {
      tonecurve[k].x = tonecurve[k + 1].x;
      tonecurve[k].y = tonecurve[k + 1].y;
    }
    c->selected = -2; // avoid re-insertion of that point immediately after this
    p->tonecurve_nodes[ch]--;
  }
}

static gboolean _move_point_internal(dt_iop_module_t *self, GtkWidget *widget, float dx, float dy, guint state)
{
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;

  int ch = c->channel;
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[ch];

  float multiplier = dt_accel_get_speed_multiplier(widget, state);
  dx *= multiplier;
  dy *= multiplier;

  tonecurve[c->selected].x = CLAMP(tonecurve[c->selected].x + dx, 0.0f, 1.0f);
  tonecurve[c->selected].y = CLAMP(tonecurve[c->selected].y + dy, 0.0f, 1.0f);

  dt_iop_tonecurve_sanity_check(self, widget);

  gtk_widget_queue_draw(widget);

  dt_iop_queue_history_update(self, FALSE);
  return TRUE;
}

#define TONECURVE_DEFAULT_STEP (0.001f)

static gboolean _scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;

  if(dt_gui_ignore_scroll(event)) return FALSE;

  int ch = c->channel;
  int autoscale_ab = p->tonecurve_autoscale_ab;

  // if autoscale_ab is on: do not modify a and b curves
  if((autoscale_ab != DT_S_SCALE_MANUAL) && ch != ch_L) return TRUE;

  if(c->selected < 0) return TRUE;

  gdouble delta_y;
  if(dt_gui_get_scroll_delta(event, &delta_y))
  {
    delta_y *= -TONECURVE_DEFAULT_STEP;
    return _move_point_internal(self, widget, 0.0, delta_y, event->state);
  }

  return TRUE;
}

static gboolean dt_iop_tonecurve_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;

  int ch = c->channel;
  int autoscale_ab = p->tonecurve_autoscale_ab;

  // if autoscale_ab is on: do not modify a and b curves
  if((autoscale_ab != DT_S_SCALE_MANUAL) && ch != ch_L) return FALSE;

  if(c->selected < 0) return FALSE;

  int handled = 0;
  float dx = 0.0f, dy = 0.0f;
  if(event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_KP_Up)
  {
    handled = 1;
    dy = TONECURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_KP_Down)
  {
    handled = 1;
    dy = -TONECURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Right || event->keyval == GDK_KEY_KP_Right)
  {
    handled = 1;
    dx = TONECURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Left || event->keyval == GDK_KEY_KP_Left)
  {
    handled = 1;
    dx = -TONECURVE_DEFAULT_STEP;
  }

  if(!handled) return FALSE;

  return _move_point_internal(self, widget, dx, dy, event->state);
}

#undef TONECURVE_DEFAULT_STEP

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_tonecurve_gui_data_t *c = IOP_GUI_ALLOC(tonecurve);
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->default_params;

  for(int ch = 0; ch < ch_max; ch++)
  {
    c->minmax_curve[ch] = dt_draw_curve_new(0.0, 1.0, p->tonecurve_type[ch]);
    c->minmax_curve_nodes[ch] = p->tonecurve_nodes[ch];
    c->minmax_curve_type[ch] = p->tonecurve_type[ch];
    for(int k = 0; k < p->tonecurve_nodes[ch]; k++)
      (void)dt_draw_curve_add_point(c->minmax_curve[ch], p->tonecurve[ch][k].x, p->tonecurve[ch][k].y);
  }

  c->channel = ch_L;
  c->mouse_x = c->mouse_y = -1.0;
  c->selected = -1;
  c->loglogscale = 0;
  c->semilog = 0;
  self->timeout_handle = 0;

  c->autoscale_ab = dt_bauhaus_combobox_from_params(self, "tonecurve_autoscale_ab");
  gtk_widget_set_tooltip_text(c->autoscale_ab, _("if set to auto, a and b curves have no effect and are "
                                                 "not displayed. chroma values (a and b) of each pixel are "
                                                 "then adjusted based on L curve data. auto XYZ is similar "
                                                 "but applies the saturation changes in XYZ space."));
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  static dt_action_def_t notebook_def = { };
  c->channel_tabs = dt_ui_notebook_new(&notebook_def);
  dt_action_define_iop(self, NULL, N_("channel"), GTK_WIDGET(c->channel_tabs), &notebook_def);
  dt_ui_notebook_page(c->channel_tabs, N_("L"), _("tonecurve for L channel"));
  dt_ui_notebook_page(c->channel_tabs, N_("a"), _("tonecurve for a channel"));
  dt_ui_notebook_page(c->channel_tabs, N_("b"), _("tonecurve for b channel"));
  g_signal_connect(G_OBJECT(c->channel_tabs), "switch_page", G_CALLBACK(tab_switch), self);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(c->channel_tabs), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), gtk_grid_new(), TRUE, TRUE, 0);

  c->colorpicker = dt_color_picker_new(self, DT_COLOR_PICKER_POINT_AREA, hbox);
  gtk_widget_set_tooltip_text(c->colorpicker, _("pick GUI color from image\nctrl+click or right-click to select an area"));

  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);

  c->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(1.0));
  g_object_set_data(G_OBJECT(c->area), "iop-instance", self);
  dt_action_define_iop(self, NULL, N_("curve"), GTK_WIDGET(c->area), NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->area), TRUE, TRUE, 0);

  // FIXME: that tooltip goes in the way of the numbers when you hover a node to get a reading
  //gtk_widget_set_tooltip_text(GTK_WIDGET(c->area), _("double click to reset curve"));

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | darktable.gui->scroll_mask
                                           | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                           | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
  gtk_widget_set_can_focus(GTK_WIDGET(c->area), TRUE);
  g_signal_connect(G_OBJECT(c->area), "draw", G_CALLBACK(dt_iop_tonecurve_draw), self);
  g_signal_connect(G_OBJECT(c->area), "button-press-event", G_CALLBACK(dt_iop_tonecurve_button_press), self);
  g_signal_connect(G_OBJECT(c->area), "motion-notify-event", G_CALLBACK(dt_iop_tonecurve_motion_notify), self);
  g_signal_connect(G_OBJECT(c->area), "leave-notify-event", G_CALLBACK(dt_iop_tonecurve_leave_notify), self);
  g_signal_connect(G_OBJECT(c->area), "enter-notify-event", G_CALLBACK(dt_iop_tonecurve_enter_notify), self);
  g_signal_connect(G_OBJECT(c->area), "configure-event", G_CALLBACK(area_resized), self);
  g_signal_connect(G_OBJECT(c->area), "scroll-event", G_CALLBACK(_scrolled), self);
  g_signal_connect(G_OBJECT(c->area), "key-press-event", G_CALLBACK(dt_iop_tonecurve_key_press), self);

  /* From src/common/curve_tools.h :
    #define CUBIC_SPLINE 0
    #define CATMULL_ROM 1
    #define MONOTONE_HERMITE 2
  */
  c->interpolator = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(c->interpolator, NULL, N_("interpolation method"));
  dt_bauhaus_combobox_add(c->interpolator, _("cubic spline"));
  dt_bauhaus_combobox_add(c->interpolator, _("centripetal spline"));
  dt_bauhaus_combobox_add(c->interpolator, _("monotonic spline"));
  gtk_box_pack_start(GTK_BOX(self->widget), c->interpolator , TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(c->interpolator, _("change this method if you see oscillations or cusps in the curve\n"
                                                 "- cubic spline is better to produce smooth curves but oscillates when nodes are too close\n"
                                                 "- centripetal is better to avoids cusps and oscillations with close nodes but is less smooth\n"
                                                 "- monotonic is better for accuracy of pure analytical functions (log, gamma, exp)\n"));
  g_signal_connect(G_OBJECT(c->interpolator), "value-changed", G_CALLBACK(interpolator_callback), self);

  c->preserve_colors = dt_bauhaus_combobox_from_params(self, "preserve_colors");
  gtk_widget_set_tooltip_text(c->preserve_colors, _("method to preserve colors when applying contrast"));

  c->logbase = dt_bauhaus_slider_new_with_range(self, 0.0f, 40.0f, 0, 0.0f, 2);
  dt_bauhaus_widget_set_label(c->logbase, NULL, N_("scale for graph"));
  gtk_box_pack_start(GTK_BOX(self->widget), c->logbase , TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(c->logbase), "value-changed", G_CALLBACK(logbase_callback), self);

  c->sizegroup = GTK_SIZE_GROUP(gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL));
  gtk_size_group_add_widget(c->sizegroup, GTK_WIDGET(c->area));
  gtk_size_group_add_widget(c->sizegroup, GTK_WIDGET(c->channel_tabs));
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  // this one we need to unref manually. not so the initially unowned widgets.
  g_object_unref(c->sizegroup);
  dt_draw_curve_destroy(c->minmax_curve[ch_L]);
  dt_draw_curve_destroy(c->minmax_curve[ch_a]);
  dt_draw_curve_destroy(c->minmax_curve[ch_b]);
  dt_iop_cancel_history_update(self);

  IOP_GUI_FREE;
}

static gboolean dt_iop_tonecurve_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_tonecurve_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static void picker_scale(const float *in, float *out)
{
  out[0] = CLAMP(in[0] / 100.0f, 0.0f, 1.0f);
  out[1] = CLAMP((in[1] + 128.0f) / 256.0f, 0.0f, 1.0f);
  out[2] = CLAMP((in[2] + 128.0f) / 256.0f, 0.0f, 1.0f);
}

static gboolean dt_iop_tonecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_global_data_t *gd = (dt_iop_tonecurve_global_data_t *)self->global_data;

  int ch = c->channel;
  int nodes = p->tonecurve_nodes[ch];
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[ch];

  if(c->minmax_curve_type[ch] != p->tonecurve_type[ch] || c->minmax_curve_nodes[ch] != p->tonecurve_nodes[ch])
  {
    dt_draw_curve_destroy(c->minmax_curve[ch]);
    c->minmax_curve[ch] = dt_draw_curve_new(0.0, 1.0, p->tonecurve_type[ch]);
    c->minmax_curve_nodes[ch] = p->tonecurve_nodes[ch];
    c->minmax_curve_type[ch] = p->tonecurve_type[ch];
    for(int k = 0; k < p->tonecurve_nodes[ch]; k++)
      (void)dt_draw_curve_add_point(c->minmax_curve[ch], p->tonecurve[ch][k].x, p->tonecurve[ch][k].y);
  }
  else
  {
    for(int k = 0; k < p->tonecurve_nodes[ch]; k++)
      dt_draw_curve_set_point(c->minmax_curve[ch], k, p->tonecurve[ch][k].x, p->tonecurve[ch][k].y);
  }
  dt_draw_curve_t *minmax_curve = c->minmax_curve[ch];
  dt_draw_curve_calc_values(minmax_curve, 0.0, 1.0, DT_IOP_TONECURVE_RES, c->draw_xs, c->draw_ys);

  float unbounded_coeffs[3];
  const float xm = tonecurve[nodes - 1].x;
  {
    const float x[4] = { 0.7f * xm, 0.8f * xm, 0.9f * xm, 1.0f * xm };
    const float y[4] = { c->draw_ys[CLAMP((int)(x[0] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)],
                         c->draw_ys[CLAMP((int)(x[1] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)],
                         c->draw_ys[CLAMP((int)(x[2] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)],
                         c->draw_ys[CLAMP((int)(x[3] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)] };
    dt_iop_estimate_exp(x, y, 4, unbounded_coeffs);
  }

  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  cairo_translate(cr, inset, inset);
  width -= 2 * inset;
  height -= 2 * inset;
  char text[256];

  // Draw frame borders
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5));
  set_color(cr, darktable.bauhaus->graph_border);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke_preserve(cr);

  if(ch==ch_L)
  { // remove below black to white transition to improve readability of the graph
    cairo_set_source_rgb(cr, .3, .3, .3);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);
  }
  else
  {
    // Draw the background gradient along the diagonal
    // ch == 0 : black to white
    // ch == 1 : green to magenta
    // ch == 2 : blue to yellow
    const float origin[3][3] = { { 0.0f, 0.0f, 0.0f },                  // L = 0, @ (a, b) = 0
                                 { 0.0f, 231.0f/255.0f, 181.0f/255.0f },// a = -128 @ L = 75, b = 0
                                 { 0.0f, 30.0f/255.0f, 195.0f/255.0f}}; // b = -128 @ L = 75, a = 0

    const float destin[3][3] = { { 1.0f, 1.0f, 1.0f },                  // L = 100 @ (a, b) = 0
                                 { 1.0f, 0.0f, 192.0f/255.0f } ,        // a = 128 @ L = 75, b = 0
                                 { 215.0f/255.0f, 182.0f/255.0f, 0.0f}};// b = 128 @ L = 75, a = 0

   // since Cairo paints with sRGB at gamma 2.4, linear gradients are not linear and, at 50%,
   // we don't see the neutral grey we would expect in the middle of a linear gradient between
   // 2 complimentary colors. So we add it artificially, but that will break the smoothness
   // of the transition.

    // middle step for gradients (50 %)
    const float midgrey = to_log(0.45f, c->loglogscale, ch, c->semilog, 0);

    const float middle[3][3] = { { midgrey, midgrey, midgrey },   // L = 50 @ (a, b) = 0
                                 { 0.67f, 0.67f, 0.67f},          // L = 75 @ (a, b) = 0
                                 { 0.67f, 0.67f, 0.67f}};         // L = 75 @ (a, b) = 0

    const float opacities[3] = { 0.5f, 0.5f, 0.5f};

    cairo_pattern_t *pat;
    pat = cairo_pattern_create_linear (height, 0.0,  0.0, width);
    cairo_pattern_add_color_stop_rgba (pat, 1, origin[ch][0], origin[ch][1], origin[ch][2], opacities[ch]);
    cairo_pattern_add_color_stop_rgba (pat, 0.5, middle[ch][0], middle[ch][1], middle[ch][2], opacities[ch]);
    cairo_pattern_add_color_stop_rgba (pat, 0, destin[ch][0], destin[ch][1], destin[ch][2], opacities[ch]);
    cairo_set_source (cr, pat);
    cairo_fill (cr);
    cairo_pattern_destroy (pat);
  }

  // draw grid
  set_color(cr, darktable.bauhaus->graph_border);

  if(c->loglogscale > 0.0f && ch == ch_L )
  {
    if(c->semilog == 0)
    {
      dt_draw_loglog_grid(cr, 4, 0, height, width, 0, c->loglogscale + 1.0f);
    }
    else if(c->semilog == 1)
    {
      dt_draw_semilog_x_grid(cr, 4, 0, height, width, 0, c->loglogscale + 1.0f);
    }
    else if(c->semilog == -1)
    {
      dt_draw_semilog_y_grid(cr, 4, 0, height, width, 0, c->loglogscale + 1.0f);
    }
  }
  else
  {
    dt_draw_grid(cr, 4, 0, 0, width, height);
  }

  // draw identity line
  cairo_move_to(cr, 0, height);
  cairo_line_to(cr, width, 0);
  cairo_stroke(cr);
  cairo_translate(cr, 0, height);

  // draw histogram in background
  // only if module is enabled
  if(self->enabled)
  {
    float *raw_mean, *raw_min, *raw_max;
    float *raw_mean_output;
    dt_aligned_pixel_t picker_mean, picker_min, picker_max;
    const gboolean is_linear = darktable.lib->proxy.histogram.is_linear;

    raw_mean = gd->picked_color;
    raw_min = gd->picked_color_min;
    raw_max = gd->picked_color_max;
    raw_mean_output = gd->picked_output_color;

    const uint32_t *hist = self->histogram;
    const float hist_max = is_linear ? self->histogram_max[ch] : logf(1.0 + self->histogram_max[ch]);
    if(hist && hist_max > 0.0f)
    {
      cairo_save(cr);
      cairo_scale(cr, width / 255.0, -(height - DT_PIXEL_APPLY_DPI(5)) / hist_max);
      cairo_move_to(cr, 0, height);
      set_color(cr, darktable.bauhaus->inset_histogram);

      if(ch == ch_L && c->loglogscale > 0.0f)
      {
        dt_draw_histogram_8_log_base(cr, hist, 4, ch, is_linear, c->loglogscale + 1.0f);
      }
      else
      {
        dt_draw_histogram_8(cr, hist, 4, ch, is_linear);
      }
      cairo_restore(cr);
    }

    cairo_move_to(cr, 0, height);
    if(self->request_color_pick == DT_REQUEST_COLORPICK_MODULE &&
       gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(c->colorpicker)))
    {
      // the global live samples ...
      cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3));

      for(GSList *samples = darktable.lib->proxy.colorpicker.live_samples; samples; samples = g_slist_next(samples))
      {
        dt_colorpicker_sample_t *sample = samples->data;

        picker_scale(sample->lab[DT_LIB_COLORPICKER_STATISTIC_MEAN], picker_mean);
        picker_scale(sample->lab[DT_LIB_COLORPICKER_STATISTIC_MIN], picker_min);
        picker_scale(sample->lab[DT_LIB_COLORPICKER_STATISTIC_MAX], picker_max);

        // Convert abcissa to log coordinates if needed
        picker_min[ch] = to_log(picker_min[ch], c->loglogscale, ch, c->semilog, 0);
        picker_max[ch] = to_log(picker_max[ch], c->loglogscale, ch, c->semilog, 0);
        picker_mean[ch] = to_log(picker_mean[ch], c->loglogscale, ch, c->semilog, 0);

        cairo_set_source_rgba(cr, 0.5, 0.7, 0.5, 0.35);
        cairo_rectangle(cr, width * picker_min[ch], 0, width * fmax(picker_max[ch] - picker_min[ch], 0.0f),
                        -height);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.5, 0.7, 0.5, 0.5);
        cairo_move_to(cr, width * picker_mean[ch], 0);
        cairo_line_to(cr, width * picker_mean[ch], -height);
        cairo_stroke(cr);
      }

      // ... and the local sample
      if(raw_max[0] >= 0.0f)
      {
        cairo_save(cr);
        PangoLayout *layout;
        PangoRectangle ink;
        PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
        pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
        pango_font_description_set_absolute_size(desc, PANGO_SCALE);
        layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, desc);

        picker_scale(raw_mean, picker_mean);
        picker_scale(raw_min, picker_min);
        picker_scale(raw_max, picker_max);

        // scale conservatively to 100% of width:
        snprintf(text, sizeof(text), "100.00 / 100.00 ( +100.00)");
        pango_layout_set_text(layout, text, -1);
        pango_layout_get_pixel_extents(layout, &ink, NULL);
        pango_font_description_set_absolute_size(desc, width*1.0/ink.width * PANGO_SCALE);
        pango_layout_set_font_description(layout, desc);

        picker_min[ch] = to_log(picker_min[ch], c->loglogscale, ch, c->semilog, 0);
        picker_max[ch] = to_log(picker_max[ch], c->loglogscale, ch, c->semilog, 0);
        picker_mean[ch] = to_log(picker_mean[ch], c->loglogscale, ch, c->semilog, 0);

        cairo_set_source_rgba(cr, 0.7, 0.5, 0.5, 0.35);
        cairo_rectangle(cr, width * picker_min[ch], 0, width * fmax(picker_max[ch] - picker_min[ch], 0.0f),
                        -height);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.9, 0.7, 0.7, 0.5);
        cairo_move_to(cr, width * picker_mean[ch], 0);
        cairo_line_to(cr, width * picker_mean[ch], -height);
        cairo_stroke(cr);

        snprintf(text, sizeof(text), "%.1f → %.1f", raw_mean[ch], raw_mean_output[ch]);

        set_color(cr, darktable.bauhaus->graph_fg);
        cairo_set_font_size(cr, DT_PIXEL_APPLY_DPI(0.04) * height);
        pango_layout_set_text(layout, text, -1);
        pango_layout_get_pixel_extents(layout, &ink, NULL);
        cairo_move_to(cr, 0.02f * width, -0.94 * height - ink.height - ink.y);
        pango_cairo_show_layout(cr, layout);
        cairo_stroke(cr);
        pango_font_description_free(desc);
        g_object_unref(layout);
        cairo_restore(cr);
      }
    }
  }

  // draw curve
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3));
  set_color(cr, darktable.bauhaus->graph_fg);

  for(int k = 0; k < DT_IOP_TONECURVE_RES; k++)
  {
    const float xx = k / (DT_IOP_TONECURVE_RES - 1.0f);
    float yy;

    if(xx > xm)
    {
      yy = dt_iop_eval_exp(unbounded_coeffs, xx);
    }
    else
    {
      yy = c->draw_ys[k];
    }

    const float x = to_log(xx, c->loglogscale, ch, c->semilog, 0),
                y = to_log(yy, c->loglogscale, ch, c->semilog, 1);

    cairo_line_to(cr, x * width, -height * y);
  }
  cairo_stroke(cr);

  // draw nodes positions
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3));
  for(int k = 0; k < nodes; k++)
  {
    const float x = to_log(tonecurve[k].x, c->loglogscale, ch, c->semilog, 0),
                y = to_log(tonecurve[k].y, c->loglogscale, ch, c->semilog, 1);

    cairo_arc(cr, x * width, -y * height, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
    set_color(cr, darktable.bauhaus->graph_fg);
    cairo_stroke_preserve(cr);
    set_color(cr, darktable.bauhaus->graph_bg);
    cairo_fill(cr);
  }

  // draw selected cursor
  if(c->selected >= 0)
  {
    // draw information about current selected node
    PangoLayout *layout;
    PangoRectangle ink;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(desc, PANGO_SCALE);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);

    // scale conservatively to 100% of width:
    snprintf(text, sizeof(text), "100.00 / 100.00 ( +100.00)");
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    pango_font_description_set_absolute_size(desc, width*1.0/ink.width * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);

    const float min_scale_value = ch == ch_L ? 0.0f : -128.0f;
    const float max_scale_value = ch == ch_L ? 100.0f : 128.0f;

    const float x_node_value = tonecurve[c->selected].x * (max_scale_value - min_scale_value) + min_scale_value;
    const float y_node_value = tonecurve[c->selected].y * (max_scale_value - min_scale_value) + min_scale_value;
    const float d_node_value = y_node_value - x_node_value;
    snprintf(text, sizeof(text), "%.1f / %.1f ( %+.1f)", x_node_value, y_node_value, d_node_value);

    set_color(cr, darktable.bauhaus->graph_fg);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, 0.98f * width - ink.width - ink.x, -0.02 * height - ink.height - ink.y);
    pango_cairo_show_layout(cr, layout);
    cairo_stroke(cr);
    pango_font_description_free(desc);
    g_object_unref(layout);

    // enlarge selected node
    set_color(cr, darktable.bauhaus->graph_fg_active);
    const float x = to_log(tonecurve[c->selected].x, c->loglogscale, ch, c->semilog, 0),
                y = to_log(tonecurve[c->selected].y, c->loglogscale, ch, c->semilog, 1);

    cairo_arc(cr, x * width, -y * height, DT_PIXEL_APPLY_DPI(6), 0, 2. * M_PI);
    cairo_fill(cr);
  }

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static inline int _add_node(dt_iop_tonecurve_node_t *tonecurve, int *nodes, float x, float y)
{
  int selected = -1;
  if(tonecurve[0].x > x)
    selected = 0;
  else
  {
    for(int k = 1; k < *nodes; k++)
    {
      if(tonecurve[k].x > x)
      {
        selected = k;
        break;
      }
    }
  }
  if(selected == -1) selected = *nodes;
  for(int i = *nodes; i > selected; i--)
  {
    tonecurve[i].x = tonecurve[i - 1].x;
    tonecurve[i].y = tonecurve[i - 1].y;
  }
  // found a new point
  tonecurve[selected].x = x;
  tonecurve[selected].y = y;
  (*nodes)++;
  return selected;
}

static gboolean dt_iop_tonecurve_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;

  int ch = c->channel;
  int nodes = p->tonecurve_nodes[ch];
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[ch];
  int autoscale_ab = p->tonecurve_autoscale_ab;

  // if autoscale_ab is on: do not modify a and b curves
  if((autoscale_ab != DT_S_SCALE_MANUAL) && ch != ch_L) goto finally;

  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
  double old_m_x = c->mouse_x;
  double old_m_y = c->mouse_y;
  c->mouse_x = event->x - inset;
  c->mouse_y = event->y - inset;

  const float mx = CLAMP(c->mouse_x, 0, width) / width;
  const float my = 1.0f - CLAMP(c->mouse_y, 0, height) / height;
  const float linx = to_lin(mx, c->loglogscale, ch, c->semilog, 0),
              liny = to_lin(my, c->loglogscale, ch, c->semilog, 1);

  if(event->state & GDK_BUTTON1_MASK)
  {
    // got a vertex selected:
    if(c->selected >= 0)
    {
      // this is used to translate mause position in loglogscale to make this behavior unified with linear scale.
      const float translate_mouse_x = old_m_x / width - to_log(tonecurve[c->selected].x, c->loglogscale, ch, c->semilog, 0);
      const float translate_mouse_y = 1 - old_m_y / height - to_log(tonecurve[c->selected].y, c->loglogscale, ch, c->semilog, 1);
      // dx & dy are in linear coordinates
      const float dx = to_lin(c->mouse_x / width - translate_mouse_x, c->loglogscale, ch, c->semilog, 0)
                       - to_lin(old_m_x / width - translate_mouse_x, c->loglogscale, ch, c->semilog, 0);
      const float dy = to_lin(1 - c->mouse_y / height - translate_mouse_y, c->loglogscale, ch, c->semilog, 1)
                       - to_lin(1 - old_m_y / height - translate_mouse_y, c->loglogscale, ch, c->semilog, 1);
      return _move_point_internal(self, widget, dx, dy, event->state);
    }
    else if(nodes < DT_IOP_TONECURVE_MAXNODES && c->selected >= -1)
    {
      // no vertex was close, create a new one!
      c->selected = _add_node(tonecurve, &p->tonecurve_nodes[ch], linx, liny);
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
  }
  else
  {
    // minimum area around the node to select it:
    float min = .04f;
    min *= min; // comparing against square
    int nearest = -1;
    for(int k = 0; k < nodes; k++)
    {
      float dist
          = (my - to_log(tonecurve[k].y, c->loglogscale, ch, c->semilog, 1)) * (my - to_log(tonecurve[k].y, c->loglogscale, ch, c->semilog, 1))
            + (mx - to_log(tonecurve[k].x, c->loglogscale, ch, c->semilog, 0)) * (mx - to_log(tonecurve[k].x, c->loglogscale, ch, c->semilog, 0));
      if(dist < min)
      {
        min = dist;
        nearest = k;
      }
    }
    c->selected = nearest;
  }
finally:
  if(c->selected >= 0) gtk_widget_grab_focus(widget);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_tonecurve_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_params_t *d = (dt_iop_tonecurve_params_t *)self->default_params;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;

  int ch = c->channel;
  int autoscale_ab = p->tonecurve_autoscale_ab;
  int nodes = p->tonecurve_nodes[ch];
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[ch];

  if(event->button == 1)
  {
    if(event->type == GDK_BUTTON_PRESS && dt_modifier_is(event->state, GDK_CONTROL_MASK)
       && nodes < DT_IOP_TONECURVE_MAXNODES && c->selected == -1)
    {
      // if we are not on a node -> add a new node at the current x of the pointer and y of the curve at that x
      const int inset = DT_GUI_CURVE_EDITOR_INSET;
      GtkAllocation allocation;
      gtk_widget_get_allocation(widget, &allocation);
      int width = allocation.width - 2 * inset;
      c->mouse_x = event->x - inset;
      c->mouse_y = event->y - inset;

      const float mx = CLAMP(c->mouse_x, 0, width) / (float)width;
      const float linx = to_lin(mx, c->loglogscale, ch, c->semilog, 0);

      // don't add a node too close to others in x direction, it can crash dt
      int selected = -1;
      if(tonecurve[0].x > mx)
        selected = 0;
      else
      {
        for(int k = 1; k < nodes; k++)
        {
          if(tonecurve[k].x > mx)
          {
            selected = k;
            break;
          }
        }
      }
      if(selected == -1) selected = nodes;
      // > 0 -> check distance to left neighbour
      // < nodes -> check distance to right neighbour
      if(!((selected > 0 && linx - tonecurve[selected - 1].x <= 0.025) ||
           (selected < nodes && tonecurve[selected].x - linx <= 0.025)))
      {
        // evaluate the curve at the current x position
        const float y = dt_draw_curve_calc_value(c->minmax_curve[ch], linx);

        if(y >= 0.0 && y <= 1.0) // never add something outside the viewport, you couldn't change it afterwards
        {
          // create a new node
          selected = _add_node(tonecurve, &p->tonecurve_nodes[ch], linx, y);

          // maybe set the new one as being selected
          float min = .04f;
          min *= min; // comparing against square
          for(int k = 0; k < nodes; k++)
          {
            float other_y = to_log(tonecurve[k].y, c->loglogscale, ch, c->semilog, 1);
            float dist = (y - other_y) * (y - other_y);
            if(dist < min) c->selected = selected;
          }

          dt_dev_add_history_item(darktable.develop, self, TRUE);
          gtk_widget_queue_draw(self->widget);
        }
      }
      return TRUE;
    }
    else if(event->type == GDK_2BUTTON_PRESS)
    {
      // reset current curve
      // if autoscale_ab is on: allow only reset of L curve
      if(!((autoscale_ab != DT_S_SCALE_MANUAL) && ch != ch_L))
      {
        p->tonecurve_nodes[ch] = d->tonecurve_nodes[ch];
        p->tonecurve_type[ch] = d->tonecurve_type[ch];
        for(int k = 0; k < d->tonecurve_nodes[ch]; k++)
        {
          p->tonecurve[ch][k].x = d->tonecurve[ch][k].x;
          p->tonecurve[ch][k].y = d->tonecurve[ch][k].y;
        }
        c->selected = -2; // avoid motion notify re-inserting immediately.
        dt_bauhaus_combobox_set(c->interpolator, p->tonecurve_type[ch_L]);
        dt_dev_add_history_item(darktable.develop, self, TRUE);
        gtk_widget_queue_draw(self->widget);
      }
      else
      {
        if(ch != ch_L)
        {
          p->tonecurve_autoscale_ab = DT_S_SCALE_MANUAL;
          c->selected = -2; // avoid motion notify re-inserting immediately.
          dt_bauhaus_combobox_set(c->autoscale_ab, 1);
          dt_dev_add_history_item(darktable.develop, self, TRUE);
          gtk_widget_queue_draw(self->widget);
        }
      }
      return TRUE;
    }
  }
  else if(event->button == 3 && c->selected >= 0)
  {
    if(c->selected == 0 || c->selected == nodes - 1)
    {
      float reset_value = c->selected == 0 ? 0 : 1;
      tonecurve[c->selected].y = tonecurve[c->selected].x = reset_value;
      gtk_widget_queue_draw(self->widget);
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      return TRUE;
    }

    for(int k = c->selected; k < nodes - 1; k++)
    {
      tonecurve[k].x = tonecurve[k + 1].x;
      tonecurve[k].y = tonecurve[k + 1].y;
    }
    tonecurve[nodes - 1].x = tonecurve[nodes - 1].y = 0;
    c->selected = -2; // avoid re-insertion of that point immediately after this
    p->tonecurve_nodes[ch]--;
    gtk_widget_queue_draw(self->widget);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return TRUE;
  }
  return FALSE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

