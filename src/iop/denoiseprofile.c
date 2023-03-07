/*
    This file is part of darktable,
    Copyright (C) 2012-2023 darktable developers.

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
#include "common/eaw.h"
#include "common/exif.h"
#include "common/imagebuf.h"
#include "common/nlmeans_core.h"
#include "common/noiseprofiles.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "dtgtk/drawingarea.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>

// which version of the non-local means code should be used?  0=old
// (this file), 1=new (src/common/nlmeans_core.c)
#define USE_NEW_IMPL_CL 0

#define REDUCESIZE 64
// number of intermediate buffers used by OpenCL code path.  Needs to
//   match value in src/common/nlmeans_core.c to correctly compute
//   tiling
#define NUM_BUCKETS 4

#define DT_IOP_DENOISE_PROFILE_INSET DT_PIXEL_APPLY_DPI(5)
#define DT_IOP_DENOISE_PROFILE_RES 64
#define DT_IOP_DENOISE_PROFILE_V8_BANDS 5
#define DT_IOP_DENOISE_PROFILE_BANDS 7

// the following fulcrum is used to help user to set shadows and
// strength parameters.  applying precondition on this value will give
// the same value even if shadows slider is changed, as strength will
// be adjusted to guarantee that.  from a user point of view, it
// separates "shadows" area from the rest of the image.
#define DT_IOP_DENOISE_PROFILE_P_FULCRUM 0.05f

typedef enum dt_iop_denoiseprofile_mode_t
{
  MODE_NLMEANS = 0,
  MODE_WAVELETS = 1,
  MODE_VARIANCE = 2,
  MODE_NLMEANS_AUTO = 3,
  MODE_WAVELETS_AUTO = 4
} dt_iop_denoiseprofile_mode_t;

typedef enum dt_iop_denoiseprofile_wavelet_mode_t
{
  MODE_RGB = 0,    // $DESCRIPTION: "RGB"
  MODE_Y0U0V0 = 1  // $DESCRIPTION: "Y0U0V0"
} dt_iop_denoiseprofile_wavelet_mode_t;

#define DT_DENOISE_PROFILE_NONE_V9 4
typedef enum dt_iop_denoiseprofile_channel_t
{
  DT_DENOISE_PROFILE_ALL = 0,
  DT_DENOISE_PROFILE_R = 1,
  DT_DENOISE_PROFILE_G = 2,
  DT_DENOISE_PROFILE_B = 3,
  DT_DENOISE_PROFILE_Y0 = 4,
  DT_DENOISE_PROFILE_U0V0 = 5,
  DT_DENOISE_PROFILE_NONE = 6
} dt_iop_denoiseprofile_channel_t;

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(11, dt_iop_denoiseprofile_params_t)

typedef struct dt_iop_denoiseprofile_params_v1_t
{
  float radius;     // search radius
  float strength;   // noise level after equalization
  float a[3], b[3]; // fit for poissonian-gaussian noise per color channel.
  dt_iop_denoiseprofile_mode_t mode; // switch between nlmeans and wavelets
} dt_iop_denoiseprofile_params_v1_t;

typedef struct dt_iop_denoiseprofile_params_v4_t
{
  float radius;     // search radius
  float strength;   // noise level after equalization
  float a[3], b[3]; // fit for poissonian-gaussian noise per color channel.
  dt_iop_denoiseprofile_mode_t mode; // switch between nlmeans and wavelets
  float x[DT_DENOISE_PROFILE_NONE_V9][DT_IOP_DENOISE_PROFILE_V8_BANDS];
  float y[DT_DENOISE_PROFILE_NONE_V9][DT_IOP_DENOISE_PROFILE_V8_BANDS]; // values to change wavelet force by frequency
} dt_iop_denoiseprofile_params_v4_t;

typedef struct dt_iop_denoiseprofile_params_v5_t
{
  float radius;                      // patch size
  float nbhood;                      // search radius
  float strength;                    // noise level after equalization
  float a[3], b[3];                  // fit for poissonian-gaussian noise per color channel.
  dt_iop_denoiseprofile_mode_t mode; // switch between nlmeans and wavelets
  float x[DT_DENOISE_PROFILE_NONE_V9][DT_IOP_DENOISE_PROFILE_V8_BANDS];
  float y[DT_DENOISE_PROFILE_NONE_V9][DT_IOP_DENOISE_PROFILE_V8_BANDS]; // values to change wavelet force by frequency
} dt_iop_denoiseprofile_params_v5_t;

typedef struct dt_iop_denoiseprofile_params_v6_t
{
  float radius;                      // patch size
  float nbhood;                      // search radius
  float strength;                    // noise level after equalization
  float scattering;                  // spread the patch search zone without increasing number of patches
  float a[3], b[3];                  // fit for poissonian-gaussian noise per color channel.
  dt_iop_denoiseprofile_mode_t mode; // switch between nlmeans and wavelets
  float x[DT_DENOISE_PROFILE_NONE_V9][DT_IOP_DENOISE_PROFILE_V8_BANDS];
  float y[DT_DENOISE_PROFILE_NONE_V9][DT_IOP_DENOISE_PROFILE_V8_BANDS]; // values to change wavelet force by frequency
} dt_iop_denoiseprofile_params_v6_t;

typedef struct dt_iop_denoiseprofile_params_v7_t
{
  float radius;                      // patch size
  float nbhood;                      // search radius
  float strength;                    // noise level after equalization
  float scattering;                  // spread the patch search zone without increasing number of patches
  float central_pixel_weight;        // increase central pixel's weight in patch comparison
  float a[3], b[3];                  // fit for poissonian-gaussian noise per color channel.
  dt_iop_denoiseprofile_mode_t mode; // switch between nlmeans and wavelets
  float x[DT_DENOISE_PROFILE_NONE_V9][DT_IOP_DENOISE_PROFILE_V8_BANDS];
  float y[DT_DENOISE_PROFILE_NONE_V9][DT_IOP_DENOISE_PROFILE_V8_BANDS]; // values to change wavelet force by frequency
  gboolean wb_adaptive_anscombe; // whether to adapt anscombe transform to wb coeffs
  // backward compatibility options
  gboolean fix_anscombe_and_nlmeans_norm;
} dt_iop_denoiseprofile_params_v7_t;

typedef struct dt_iop_denoiseprofile_params_v8_t
{
  float radius;     // patch size
  float nbhood;     // search radius
  float strength;   // noise level after equalization
  float shadows;    // control the impact on shadows
  float bias;       // allows to reduce backtransform bias
  float scattering; // spread the patch search zone without increasing number of patches
  float central_pixel_weight; // increase central pixel's weight in patch comparison
  float overshooting; // adjusts the way parameters are autoset
  float a[3], b[3]; // fit for poissonian-gaussian noise per color channel.
  dt_iop_denoiseprofile_mode_t mode; // switch between nlmeans and wavelets
  float x[DT_DENOISE_PROFILE_NONE_V9][DT_IOP_DENOISE_PROFILE_V8_BANDS];
  float y[DT_DENOISE_PROFILE_NONE_V9][DT_IOP_DENOISE_PROFILE_V8_BANDS]; // values to change wavelet force by frequency
  gboolean wb_adaptive_anscombe; // whether to adapt anscombe transform to wb coeffs
  // backward compatibility options
  gboolean fix_anscombe_and_nlmeans_norm;
  gboolean use_new_vst;
} dt_iop_denoiseprofile_params_v8_t;

typedef struct dt_iop_denoiseprofile_params_v9_t
{
  float radius;     // patch size
  float nbhood;     // search radius
  float strength;   // noise level after equalization
  float shadows;    // control the impact on shadows
  float bias;       // allows to reduce backtransform bias
  float scattering; // spread the patch search zone without increasing number of patches
  float central_pixel_weight; // increase central pixel's weight in patch comparison
  float overshooting; // adjusts the way parameters are autoset
  float a[3], b[3]; // fit for poissonian-gaussian noise per color channel.
  dt_iop_denoiseprofile_mode_t mode; // switch between nlmeans and wavelets
  float x[DT_DENOISE_PROFILE_NONE_V9][DT_IOP_DENOISE_PROFILE_BANDS];
  float y[DT_DENOISE_PROFILE_NONE_V9][DT_IOP_DENOISE_PROFILE_BANDS]; // values to change wavelet force by frequency
  gboolean wb_adaptive_anscombe; // whether to adapt anscombe transform to wb coeffs
  // backward compatibility options
  gboolean fix_anscombe_and_nlmeans_norm;
  gboolean use_new_vst;
} dt_iop_denoiseprofile_params_v9_t;

typedef struct dt_iop_denoiseprofile_params_v10_t
{
  float radius;     /* patch size
                       $MIN: 0.0 $MAX: 12.0 $DEFAULT: 1.0 $DESCRIPTION: "patch size" */
  float nbhood;     /* search radius
                       $MIN: 1.0 $MAX: 30.0 $DEFAULT: 7.0 $DESCRIPTION: "search radius" */
  float strength;   /* noise level after equalization
                       $MIN: 0.001 $MAX: 1000.0 $DEFAULT: 1.0 */
  float shadows;    /* control the impact on shadows
                       $MIN: 0.0 $MAX: 1.8 $DEFAULT: 1.0 $DESCRIPTION: "preserve shadows" */
  float bias;       /* allows to reduce backtransform bias
                       $MIN: -1000.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "bias correction" */
  float scattering; /* spread the patch search zone without increasing number of patches
                       $MIN: 0.0 $MAX: 20.0 $DEFAULT: 0.0 $DESCRIPTION: "scattering" */
  float central_pixel_weight; /* increase central pixel's weight in patch comparison
                       $MIN: 0.0 $MAX: 10.0 $DEFAULT: 0.1 $DESCRIPTION: "central pixel weight" */
  float overshooting; /* adjusts the way parameters are autoset
                         $MIN: 0.001 $MAX: 1000.0 $DEFAULT: 1.0 $DESCRIPTION: "adjust autoset parameters" */
  float a[3], b[3]; // fit for poissonian-gaussian noise per color channel.
  dt_iop_denoiseprofile_mode_t mode; /* switch between nlmeans and wavelets
                                        $DEFAULT: MODE_NLMEANS */
  float x[DT_DENOISE_PROFILE_NONE][DT_IOP_DENOISE_PROFILE_BANDS];
  float y[DT_DENOISE_PROFILE_NONE][DT_IOP_DENOISE_PROFILE_BANDS]; /* values to change wavelet force by frequency
                                                                     $DEFAULT: 0.5 */
  gboolean wb_adaptive_anscombe; // $DEFAULT: TRUE $DESCRIPTION: "whitebalance-adaptive transform" whether to adapt anscombe transform to wb coeffs
  gboolean fix_anscombe_and_nlmeans_norm; // $DEFAULT: TRUE $DESCRIPTION: "fix various bugs in algorithm" backward compatibility options
  gboolean use_new_vst; // $DEFAULT: TRUE $DESCRIPTION: "upgrade profiled transform" backward compatibility options
  dt_iop_denoiseprofile_wavelet_mode_t wavelet_color_mode; /* switch between RGB and Y0U0V0 modes.
                                                              $DEFAULT: MODE_Y0U0V0 $DESCRIPTION: "color mode"*/
} dt_iop_denoiseprofile_params_v10_t;

typedef struct dt_iop_denoiseprofile_params_t
{
  float radius;     /* patch size
                       $MIN: 0.0 $MAX: 12.0 $DEFAULT: 1.0 $DESCRIPTION: "patch size" */
  float nbhood;     /* search radius
                       $MIN: 1.0 $MAX: 30.0 $DEFAULT: 7.0 $DESCRIPTION: "search radius" */
  float strength;   /* noise level after equalization
                       $MIN: 0.001 $MAX: 1000.0 $DEFAULT: 1.0 */
  float shadows;    /* control the impact on shadows
                       $MIN: 0.0 $MAX: 1.8 $DEFAULT: 1.0 $DESCRIPTION: "preserve shadows" */
  float bias;       /* allows to reduce backtransform bias
                       $MIN: -1000.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "bias correction" */
  float scattering; /* spread the patch search zone without increasing number of patches
                       $MIN: 0.0 $MAX: 20.0 $DEFAULT: 0.0 $DESCRIPTION: "scattering" */
  float central_pixel_weight; /* increase central pixel's weight in patch comparison
                       $MIN: 0.0 $MAX: 10.0 $DEFAULT: 0.1 $DESCRIPTION: "central pixel weight" */
  float overshooting; /* adjusts the way parameters are autoset
                         $MIN: 0.001 $MAX: 1000.0 $DEFAULT: 1.0 $DESCRIPTION: "adjust autoset parameters" */
  float a[3], b[3]; // fit for poissonian-gaussian noise per color channel.
  dt_iop_denoiseprofile_mode_t mode; /* switch between nlmeans and wavelets
                                        $DEFAULT: MODE_WAVELETS */
  float x[DT_DENOISE_PROFILE_NONE][DT_IOP_DENOISE_PROFILE_BANDS];
  float y[DT_DENOISE_PROFILE_NONE][DT_IOP_DENOISE_PROFILE_BANDS]; /* values to change wavelet force by frequency
                                                                     $DEFAULT: 0.5 */
  gboolean wb_adaptive_anscombe; // $DEFAULT: TRUE $DESCRIPTION: "whitebalance-adaptive transform" whether to adapt anscombe transform to wb coeffs
  gboolean fix_anscombe_and_nlmeans_norm; // $DEFAULT: TRUE $DESCRIPTION: "fix various bugs in algorithm" backward compatibility options
  gboolean use_new_vst; // $DEFAULT: TRUE $DESCRIPTION: "upgrade profiled transform" backward compatibility options
  dt_iop_denoiseprofile_wavelet_mode_t wavelet_color_mode; /* switch between RGB and Y0U0V0 modes.
                                                              $DEFAULT: MODE_Y0U0V0 $DESCRIPTION: "color mode"*/
} dt_iop_denoiseprofile_params_t;

typedef struct dt_iop_denoiseprofile_gui_data_t
{
  GtkWidget *profile;
  GtkWidget *mode;
  GtkWidget *radius;
  GtkWidget *nbhood;
  GtkWidget *strength;
  GtkWidget *shadows;
  GtkWidget *bias;
  GtkWidget *scattering;
  GtkWidget *central_pixel_weight;
  GtkWidget *overshooting;
  GtkWidget *wavelet_color_mode;
  dt_noiseprofile_t interpolated; // don't use name, maker or model, they may point to garbage
  GList *profiles;
  GtkWidget *box_nlm;
  GtkWidget *box_wavelets;
  GtkWidget *box_variance;
  dt_draw_curve_t *transition_curve; // curve for gui to draw
  GtkDrawingArea *area;
  GtkNotebook *channel_tabs;
  GtkNotebook *channel_tabs_Y0U0V0;
  double mouse_x, mouse_y, mouse_pick;
  float mouse_radius;
  dt_iop_denoiseprofile_params_t drag_params;
  int dragging;
  int x_move;
  dt_iop_denoiseprofile_channel_t channel;
  float draw_xs[DT_IOP_DENOISE_PROFILE_RES], draw_ys[DT_IOP_DENOISE_PROFILE_RES];
  float draw_min_xs[DT_IOP_DENOISE_PROFILE_RES], draw_min_ys[DT_IOP_DENOISE_PROFILE_RES];
  float draw_max_xs[DT_IOP_DENOISE_PROFILE_RES], draw_max_ys[DT_IOP_DENOISE_PROFILE_RES];
  GtkWidget *wb_adaptive_anscombe;
  GtkLabel *label_var;
  float variance_R;
  GtkLabel *label_var_R;
  float variance_G;
  GtkLabel *label_var_G;
  float variance_B;
  GtkLabel *label_var_B;
  // backward compatibility options
  GtkWidget *fix_anscombe_and_nlmeans_norm;
  GtkWidget *use_new_vst;
} dt_iop_denoiseprofile_gui_data_t;

typedef struct dt_iop_denoiseprofile_data_t
{
  float radius;                      // patch radius
  float nbhood;                      // search radius
  float strength;                    // noise level after equalization
  float shadows;                     // controls noise reduction in shadows
  float bias;                        // controls bias in backtransform
  float scattering;                  // spread the search zone without changing number of patches
  float central_pixel_weight;        // increase central pixel's weight in patch comparison
  float overshooting;                // adjusts the way parameters are autoset
  float a[3], b[3];                  // fit for poissonian-gaussian noise per color channel.
  dt_iop_denoiseprofile_mode_t mode; // switch between nlmeans and wavelets
  dt_draw_curve_t *curve[DT_DENOISE_PROFILE_NONE];
  dt_iop_denoiseprofile_channel_t channel;
  float force[DT_DENOISE_PROFILE_NONE][DT_IOP_DENOISE_PROFILE_BANDS];
  gboolean wb_adaptive_anscombe;          // whether to adapt anscombe transform to wb coeffs
  gboolean fix_anscombe_and_nlmeans_norm; // backward compatibility options
  gboolean use_new_vst;                   // backward compatibility options
  dt_iop_denoiseprofile_wavelet_mode_t wavelet_color_mode; // switch between RGB and Y0U0V0 modes.
} dt_iop_denoiseprofile_data_t;

typedef struct dt_iop_denoiseprofile_global_data_t
{
  int kernel_denoiseprofile_precondition;
  int kernel_denoiseprofile_precondition_v2;
  int kernel_denoiseprofile_precondition_Y0U0V0;
  int kernel_denoiseprofile_init;
  int kernel_denoiseprofile_dist;
  int kernel_denoiseprofile_horiz;
  int kernel_denoiseprofile_vert;
  int kernel_denoiseprofile_accu;
  int kernel_denoiseprofile_finish;
  int kernel_denoiseprofile_finish_v2;
  int kernel_denoiseprofile_backtransform;
  int kernel_denoiseprofile_backtransform_v2;
  int kernel_denoiseprofile_backtransform_Y0U0V0;
  int kernel_denoiseprofile_decompose;
  int kernel_denoiseprofile_synthesize;
  int kernel_denoiseprofile_reduce_first;
  int kernel_denoiseprofile_reduce_second;
} dt_iop_denoiseprofile_global_data_t;

static dt_noiseprofile_t dt_iop_denoiseprofile_get_auto_profile(dt_iop_module_t *self);

static void debug_dump_PFM(const dt_dev_pixelpipe_iop_t *const piece,
                           const char *const namespec,
                           const float* const restrict buf,
                           const int width,
                           const int height,
                           const int scale)
{
  if(!darktable.dump_pfm_module) return;
  if((piece->pipe->type & DT_DEV_PIXELPIPE_FULL) == 0) return;

  char name[256];
  snprintf(name, sizeof(name), namespec, scale);
  dt_dump_pfm(name, buf, width, height,  4 * sizeof(float), "denoiseprofile");
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void *new_params,
                  const int new_version)
{
  if((old_version == 1 || old_version == 2 || old_version == 3) && new_version == 4)
  {
    const dt_iop_denoiseprofile_params_v1_t *o = old_params;
    dt_iop_denoiseprofile_params_v4_t *n = new_params;
    if(old_version == 1)
    {
      n->mode = MODE_NLMEANS;
    }
    else
    {
      n->mode = o->mode;
    }
    n->radius = o->radius;
    n->strength = o->strength;
    memcpy(n->a, o->a, sizeof(float) * 3);
    memcpy(n->b, o->b, sizeof(float) * 3);
    // init curves coordinates
    for(int b = 0; b < DT_IOP_DENOISE_PROFILE_V8_BANDS; b++)
    {
      for(int c = 0; c < DT_DENOISE_PROFILE_NONE_V9; c++)
      {
        n->x[c][b] = b / (DT_IOP_DENOISE_PROFILE_V8_BANDS - 1.0f);
        n->y[c][b] = 0.5f;
      }
    }
    // autodetect current profile:
    if(!self->dev)
    {
      // we are probably handling a style or preset, do nothing for
      // them, we can't do anything to detect if autodetection was
      // used or not
      return 0;
    }
    dt_noiseprofile_t interpolated = dt_iop_denoiseprofile_get_auto_profile(self);
    // if the profile in old_version is an autodetected one (this
    // would mean a+b params match the interpolated one, AND the
    // profile is actually the first selected one - however we can
    // only detect the params, but most people did probably not set
    // the exact ISO on purpose instead of the "found match" - they
    // probably still want autodetection!)
    if(!memcmp(interpolated.a, o->a, sizeof(float) * 3)
       && !memcmp(interpolated.b, o->b, sizeof(float) * 3))
    {
      // set the param a[0] to -1.0 to signal the autodetection
      n->a[0] = -1.0f;
    }
    return 0;
  }
  else if(new_version == 5)
  {
    dt_iop_denoiseprofile_params_v4_t v4;
    if(old_version < 4)
    {
      // first update to v4
      if(legacy_params(self, old_params, old_version, &v4, 4))
        return 1;
    }
    else
      memcpy(&v4, old_params, sizeof(v4)); // was v4 already

    dt_iop_denoiseprofile_params_v5_t *v5 = new_params;
    v5->radius = v4.radius;
    v5->strength = v4.strength;
    v5->mode = v4.mode;
    for(int k=0;k<3;k++)
    {
      v5->a[k] = v4.a[k];
      v5->b[k] = v4.b[k];
    }
    for(int b = 0; b < DT_IOP_DENOISE_PROFILE_V8_BANDS; b++)
    {
      for(int c = 0; c < DT_DENOISE_PROFILE_NONE_V9; c++)
      {
        v5->x[c][b] = v4.x[c][b];
        v5->y[c][b] = v4.y[c][b];
      }
    }
    v5->nbhood = 7; // set to old hardcoded default
    return 0;
  }
  else if(new_version == 6)
  {
    dt_iop_denoiseprofile_params_v5_t v5;
    if(old_version < 5)
    {
      // first update to v5
      if(legacy_params(self, old_params, old_version, &v5, 5)) return 1;
    }
    else
      memcpy(&v5, old_params, sizeof(v5)); // was v5 already

    dt_iop_denoiseprofile_params_v6_t *v6 = new_params;
    v6->radius = v5.radius;
    v6->strength = v5.strength;
    v6->mode = v5.mode;
    v6->nbhood = v5.nbhood;
    for(int k = 0; k < 3; k++)
    {
      v6->a[k] = v5.a[k];
      v6->b[k] = v5.b[k];
    }
    for(int b = 0; b < DT_IOP_DENOISE_PROFILE_V8_BANDS; b++)
    {
      for(int c = 0; c < DT_DENOISE_PROFILE_NONE_V9; c++)
      {
        v6->x[c][b] = v5.x[c][b];
        v6->y[c][b] = v5.y[c][b];
      }
    }
    v6->scattering = 0.0; // no scattering
    return 0;
  }
  else if(new_version == 7)
  {
    dt_iop_denoiseprofile_params_v6_t v6;
    if(old_version < 6)
    {
      // first update to v6
      if(legacy_params(self, old_params, old_version, &v6, 6)) return 1;
    }
    else
      memcpy(&v6, old_params, sizeof(v6)); // was v6 already
    dt_iop_denoiseprofile_params_v7_t *v7 = new_params;
    v7->radius = v6.radius;
    v7->strength = v6.strength;
    v7->mode = v6.mode;
    v7->nbhood = v6.nbhood;
    for(int k = 0; k < 3; k++)
    {
      v7->a[k] = v6.a[k];
      v7->b[k] = v6.b[k];
    }
    for(int b = 0; b < DT_IOP_DENOISE_PROFILE_V8_BANDS; b++)
    {
      for(int c = 0; c < DT_DENOISE_PROFILE_NONE_V9; c++)
      {
        v7->x[c][b] = v6.x[c][b];
        v7->y[c][b] = v6.y[c][b];
      }
    }
    v7->scattering = v6.scattering;
    v7->central_pixel_weight = 0.0;
    v7->fix_anscombe_and_nlmeans_norm = FALSE; // don't fix anscombe
                                               // and norm to ensure
                                               // backward
                                               // compatibility
    v7->wb_adaptive_anscombe = TRUE;
    return 0;
  }
  else if(new_version == 8)
  {
    dt_iop_denoiseprofile_params_v7_t v7;
    if(old_version < 7)
    {
      // first update to v7
      if(legacy_params(self, old_params, old_version, &v7, 7)) return 1;
    }
    else
      memcpy(&v7, old_params, sizeof(v7)); // was v7 already
    dt_iop_denoiseprofile_params_v8_t *v8 = new_params;
    v8->radius = v7.radius;
    v8->strength = v7.strength;
    v8->mode = v7.mode;
    v8->nbhood = v7.nbhood;
    for(int k = 0; k < 3; k++)
    {
      v8->a[k] = v7.a[k];
      v8->b[k] = v7.b[k];
    }
    for(int b = 0; b < DT_IOP_DENOISE_PROFILE_V8_BANDS; b++)
    {
      for(int c = 0; c < DT_DENOISE_PROFILE_NONE_V9; c++)
      {
        v8->x[c][b] = v7.x[c][b];
        v8->y[c][b] = v7.y[c][b];
      }
    }
    v8->scattering = v7.scattering;
    v8->central_pixel_weight = v7.central_pixel_weight;
    v8->fix_anscombe_and_nlmeans_norm = v7.fix_anscombe_and_nlmeans_norm;
    v8->wb_adaptive_anscombe = v7.wb_adaptive_anscombe;
    v8->shadows = 1.0f;
    v8->bias = 0.0f;
    v8->use_new_vst = FALSE;
    v8->overshooting = 1.0f;
    return 0;
  }
  else if(new_version == 9)
  {
    dt_iop_denoiseprofile_params_v8_t v8;
    if(old_version < 8)
    {
      // first update to v8
      if(legacy_params(self, old_params, old_version, &v8, 8)) return 1;
    }
    else
      memcpy(&v8, old_params, sizeof(v8)); // was v8 already
    dt_iop_denoiseprofile_params_t *v9 = new_params;
    v9->radius = v8.radius;
    v9->strength = v8.strength;
    v9->mode = v8.mode;
    v9->nbhood = v8.nbhood;
    for(int k = 0; k < 3; k++)
    {
      v9->a[k] = v8.a[k];
      v9->b[k] = v8.b[k];
    }
    for(int b = 0; b < DT_IOP_DENOISE_PROFILE_BANDS; b++)
    {
      for(int c = 0; c < DT_DENOISE_PROFILE_NONE_V9; c++)
      {
        v9->x[c][b] = b / (DT_IOP_DENOISE_PROFILE_BANDS - 1.0f);
        v9->y[c][b] = 0.0f;
      }
    }
    for(int b = 0; b < DT_IOP_DENOISE_PROFILE_V8_BANDS; b++)
    {
      for(int c = 0; c < DT_DENOISE_PROFILE_NONE_V9; c++)
      {
        v9->y[c][b + DT_IOP_DENOISE_PROFILE_BANDS - DT_IOP_DENOISE_PROFILE_V8_BANDS] =
          v8.y[c][b];
      }
    }
    v9->scattering = v8.scattering;
    v9->central_pixel_weight = v8.central_pixel_weight;
    v9->fix_anscombe_and_nlmeans_norm = v8.fix_anscombe_and_nlmeans_norm;
    v9->wb_adaptive_anscombe = v8.wb_adaptive_anscombe;
    v9->shadows = v8.shadows;
    v9->bias = v8.bias;
    v9->use_new_vst = v8.use_new_vst;
    v9->overshooting = v8.overshooting;
    return 0;
  }
  else if(new_version == 10)
  {
    dt_iop_denoiseprofile_params_t v9;
    if(old_version < 9)
    {
      // first update to v9
      if(legacy_params(self, old_params, old_version, &v9, 9)) return 1;
    }
    else
      memcpy(&v9, old_params, sizeof(v9)); // was v9 already
    dt_iop_denoiseprofile_params_t *v10 = new_params;

    // start with a clean default
    dt_iop_denoiseprofile_params_t *d = self->default_params;
    *v10 = *d;

    v10->radius = v9.radius;
    v10->strength = v9.strength;
    v10->mode = v9.mode;
    v10->nbhood = v9.nbhood;
    for(int k = 0; k < 3; k++)
    {
      v10->a[k] = v9.a[k];
      v10->b[k] = v9.b[k];
    }
    for(int b = 0; b < DT_IOP_DENOISE_PROFILE_BANDS; b++)
    {
      for(int c = 0; c < DT_DENOISE_PROFILE_NONE_V9; c++)
      {
        v10->x[c][b] = v9.x[c][b];
        v10->y[c][b] = v9.y[c][b];
      }
      for(int c = DT_DENOISE_PROFILE_NONE_V9; c < DT_DENOISE_PROFILE_NONE; c++)
      {
        v10->x[c][b] = b / (DT_IOP_DENOISE_PROFILE_BANDS - 1.0f);
        v10->y[c][b] = 0.5f;
      }
    }
    v10->scattering = v9.scattering;
    v10->central_pixel_weight = v9.central_pixel_weight;
    v10->fix_anscombe_and_nlmeans_norm = v9.fix_anscombe_and_nlmeans_norm;
    v10->wb_adaptive_anscombe = v9.wb_adaptive_anscombe;
    v10->shadows = v9.shadows;
    v10->bias = v9.bias;
    v10->use_new_vst = v9.use_new_vst;
    v10->overshooting = v9.overshooting;
    v10->wavelet_color_mode = MODE_RGB;
    return 0;
  }
  else if(new_version == 11)
  {
    // v11 and v10 are the same, just need to update strength when needed.
    dt_iop_denoiseprofile_params_t *v11 = new_params;
    if(old_version < 10)
    {
      if(legacy_params(self, old_params, old_version, v11, 10)) return 1;
    }
    else
      memcpy(v11, old_params, sizeof(*v11)); // was v10 already

    if((v11->mode == MODE_WAVELETS
        || v11->mode == MODE_WAVELETS_AUTO)
       && v11->wavelet_color_mode == MODE_Y0U0V0)
    {
      // in Y0U0V0, in v11, we always increase strength in the algorithm, so that
      // the amount of smoothing is closer to what we get with the other modes.
      const float compensate_strength = 2.5f;
      v11->strength /= compensate_strength;
    }
    return 0;
  }
  return 1;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_denoiseprofile_params_t p;
  memset(&p, 0, sizeof(p));

  // set some default values
  p.radius = 1.0;
  p.nbhood = 7.0;

  // then the wavelet ones
  p.mode = MODE_WAVELETS;
  p.wavelet_color_mode = MODE_Y0U0V0;
  p.strength = 1.2f;
  p.use_new_vst = TRUE;
  // disable variance stabilization transform to avoid any bias
  // (wavelets perform well even without the VST):
  p.shadows = 0.0f;
  p.bias = 0.0f;
  // this influences as well the way Y0U0V0 is computed:
  p.wb_adaptive_anscombe = TRUE;
  p.a[0] = -1.0f; // autodetect profile
  p.central_pixel_weight = 0.1f;
  p.overshooting = 1.0f;
  p.fix_anscombe_and_nlmeans_norm = TRUE;
  for(int b = 0; b < DT_IOP_DENOISE_PROFILE_BANDS; b++)
  {
    for(int c = 0; c < DT_DENOISE_PROFILE_NONE; c++)
    {
      p.x[c][b] = b / (DT_IOP_DENOISE_PROFILE_BANDS - 1.0f);
      p.y[c][b] = 0.5f;
    }
    p.x[DT_DENOISE_PROFILE_Y0][b] = b / (DT_IOP_DENOISE_PROFILE_BANDS - 1.0f);
    p.y[DT_DENOISE_PROFILE_Y0][b] = 0.0f;
  }
  dt_gui_presets_add_generic(_("wavelets: chroma only"), self->op, 11, &p,
                             sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);
}

const char *name()
{
  return _("denoise (profiled)");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("denoise using noise statistics profiled on sensors"),
                                _("corrective"),
                                _("linear, RGB, scene-referred"),
                                _("linear, RGB"),
                                _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self,
                       dt_dev_pixelpipe_t *pipe,
                       dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

typedef union floatint_t
{
  float f;
  uint32_t i;
} floatint_t;

void tiling_callback(struct dt_iop_module_t *self,
                     struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_denoiseprofile_params_t *d = (dt_iop_denoiseprofile_params_t *)piece->data;

  if(d->mode == MODE_NLMEANS || d->mode == MODE_NLMEANS_AUTO)
  {
    // pixel filter size:
    const int P = ceilf(d->radius * fminf(fminf(roi_in->scale, 2.0f)
                                          / fmaxf(piece->iscale, 1.0f), 1.0f));
    const int K = ceilf(d->nbhood * fminf(fminf(roi_in->scale, 2.0f)
                                          / fmaxf(piece->iscale, 1.0f), 1.0f)); // nbhood
    const int K_scattered = ceilf(d->scattering
                                  * (K * K * K + 7.0 * K * sqrt(K)) / 6.0) + K;

    tiling->factor = 2.0f + 0.25f; // in + out + tmp
    // in + out + (2 + NUM_BUCKETS * 0.25) tmp:
    tiling->factor_cl = 4.0f + 0.25f * NUM_BUCKETS;
    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    tiling->overlap = P + K_scattered;
    tiling->xalign = 1;
    tiling->yalign = 1;
  }
  else
  {
    const int max_max_scale = DT_IOP_DENOISE_PROFILE_BANDS; // hard limit
    int max_scale = 0;
    const float scale = fminf(roi_in->scale / piece->iscale, 1.0f);
    // largest desired filter on input buffer (20% of input dim)
    const float supp0
        = fminf(2 * (2u << (max_max_scale - 1)) + 1,
              fmaxf(piece->buf_in.height * piece->iscale,
                    piece->buf_in.width * piece->iscale) * 0.2f);
    const float i0 = dt_log2f((supp0 - 1.0f) * .5f);

    for(; max_scale < max_max_scale; max_scale++)
    {
      // actual filter support on scaled buffer
      const float supp = 2 * (2u << max_scale) + 1;
      // approximates this filter size on unscaled input image:
      const float supp_in = supp * (1.0f / scale);
      const float i_in = dt_log2f((supp_in - 1) * .5f) - 1.0f;
      // i_in = max_scale .. .. .. 0
      const float t = 1.0f - (i_in + .5f) / i0;
      if(t < 0.0f) break;
    }

    const int max_filter_radius = (1u << max_scale); // 2 * 2^max_scale

    tiling->factor = 5.0f; // in + out + precond + tmp + reducebuffer
    tiling->factor_cl = 3.5f + max_scale; // in + out + tmp + reducebuffer + scale buffers
    tiling->maxbuf = 1.0f;
    tiling->maxbuf_cl = 1.0f;
    tiling->overhead = 0;
    tiling->overlap = max_filter_radius;
    tiling->xalign = 1;
    tiling->yalign = 1;
  }

}

static inline void precondition(const float *const in,
                                float *const buf,
                                const int wd,
                                const int ht,
                                const dt_aligned_pixel_t a,
                                const dt_aligned_pixel_t b)
{
  const dt_aligned_pixel_t sigma2_plus_3_8
      = { (b[0] / a[0]) * (b[0] / a[0]) + 3.f / 8.f,
          (b[1] / a[1]) * (b[1] / a[1]) + 3.f / 8.f,
          (b[2] / a[2]) * (b[2] / a[2]) + 3.f / 8.f,
          0.0f };
  const size_t npixels = (size_t)wd * ht;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(buf, npixels, in, sigma2_plus_3_8) \
  shared(a) \
  schedule(static)
#endif
  for(size_t j = 0; j < 4U * npixels; j += 4)
  {
    for_each_channel(c,aligned(in,buf,a,sigma2_plus_3_8))
    {
      const float d = fmaxf(0.0f, in[j+c] / a[c] + sigma2_plus_3_8[c]);
      buf[j+c] = 2.0f * sqrtf(d);
    }
  }
}

static inline void backtransform(float *const buf,
                                 const int wd,
                                 const int ht,
                                 const dt_aligned_pixel_t a,
                                 const dt_aligned_pixel_t b)
{
  const dt_aligned_pixel_t sigma2_plus_1_8
      = { (b[0] / a[0]) * (b[0] / a[0]) + 1.f / 8.f,
          (b[1] / a[1]) * (b[1] / a[1]) + 1.f / 8.f,
          (b[2] / a[2]) * (b[2] / a[2]) + 1.f / 8.f,
          0.0f };
  const size_t npixels = (size_t)wd * ht;
  const float sqrt_3_2 = sqrtf(3.0f / 2.0f);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(buf, npixels, sigma2_plus_1_8, sqrt_3_2)   \
  shared(a) \
  schedule(static)
#endif
  for(size_t j = 0; j < 4U * npixels; j += 4)
  {
    for_each_channel(c,aligned(buf,sigma2_plus_1_8))
    {
      const float x = buf[j+c], x2 = x * x;
      // closed form approximation to unbiased inverse (input range
      // was 0..200 for fit, not 0..1)
      buf[j+c] = (x < 0.5f)
        ? 0.0f
        : a[c] * (1.f / 4.f * x2 + 1.f / 4.f * sqrt_3_2 / x - 11.f / 8.f / x2
                  + 5.f / 8.f * sqrt_3_2 / (x * x2) - sigma2_plus_1_8[c]);
      // asymptotic form:
      // buf[j+c] = fmaxf(0.0f, 1./4.*x*x - 1./8. - sigma2[c]);
      // buf[j+c] *= a[c];
    }
  }
}

// the "v2" variance stabilizing transform is an extension of the generalized
// anscombe transform.
// In the generalized anscombe transform, the profiles gives a and b such as:
// V(X) = a * E[X] + b
// In this new transform, we have an additional parameter, p, such as:
// V(X) = a * (E[X] + b) ^ p
// When p == 1, we get back the equation of generalized anscombe transform.
// Now, let's see how we derive the precondition.
// The goal of a VST f is to make variance constant: V(f(X)) = constant
// Using a Taylor expansion, we have:
// V(f(X)) ~= V(f(E[X])+f'(X)(X-E[X]))
//          = V(f'(X)(X-E[X]))
//          = f'(X)^2 * V(X-E[X])
//          = f'(X)^2 * V(X)
// So the condition V(f(X)) = constant gives us the following condition:
// V(X) = constant / f'(X)^2
// Usually, we take constant = 1
// If we have V(X) = a * (E[X] + b) ^ p
// then: f'(X) = 1 / sqrt(a) * (E[X] + b) ^ (-p / 2)
// then: f(x) = 1 / (sqrt(a) * (1 - p / 2)) * (x + b) ^ (1 - p / 2)
//            = 2 * (x + b) ^ (1 - p / 2) / (sqrt(a) * (2 - p))
// is a suitable function.
// This is the function we use here.
static inline void precondition_v2(const float *const in,
                                   float *const buf,
                                   const int wd,
                                   const int ht,
                                   const float a,
                                   const dt_aligned_pixel_t p,
                                   const float b,
                                   const dt_aligned_pixel_t wb)
{
  const size_t npixels = (size_t)wd * ht;
  const dt_aligned_pixel_t expon = { -p[0] / 2 + 1, -p[1] / 2 + 1, -p[2] / 2 + 1, 1.0f };
  const dt_aligned_pixel_t denom = { (-p[0] + 2) * sqrtf(a), (-p[1] + 2) * sqrtf(a),
                                     (-p[2] + 2) * sqrtf(a), 1.0f };

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels, buf, in, b, wb) \
  dt_omp_sharedconst(expon, denom) \
  schedule(static)
#endif
  for(size_t j = 0; j < 4U * npixels; j += 4)
  {
    for_each_channel(c,aligned(in,buf,wb))
    {
      buf[j+c] = 2.0f * powf(MAX(in[j+c] / wb[c] + b, 0.0f), expon[c]) / denom[c];
    }
  }
}

// this backtransform aims at being a low bias backtransform
// you can see that it is not equal to f-1
// this is because E[X] != f-1(E[f(X)])
// so let's try to find a better backtransform than f-1:
// we want to find E[X] knowing E[f(X)]
// let's apply Taylor expansion to E[f(X)] to see if we can get something better:
// E[f(X)] ~= E[f(E[X]) + f'(E[X])(X-E[X])]
//          = E[f(E[X]) + f'(E[X]) * X - f'(E[X]) * E[X]]
//          = f(E[X]) + f'(E[X]) * E[X] - f'(E[X]) * E[X]
//          = f(E[X])
// so first order Taylor expansion is not useful.
// going to the second order:
// E[f(X)] ~= E[f(E[X]) + f'(E[X])(X-E[X]) + f"(E[X])/2 * (X-E[X])^2]
//          = f(E[X]) + f"(E[X])/2 * E[(X-E[X])^2]
//          = f(E[X]) + f"(E[X])/2 * V(X)
// and we know that V(X) = constant / f'(X)^2
// the constant here is not 1, due to problems in noise profiling tool
// so in fact constant depends on the image (and is approximately in [10;15])
// so:
// E[f(X)] ~= f(E[X]) + f"(E[X])/2 * constant / f'(E[X])^2
// we have:
// f(x) = 2 * (x + b) ^ (1 - p / 2) / (sqrt(a) * (2 - p))
// f'(x) = 1 / sqrt(a) * (x + b) ^ (-p / 2)
// 1/f'(x)^2 = a * (x + b) ^ p
// f"(x) = 1 / sqrt(a) * (-p / 2) * (x + b) ^ (- p / 2 - 1)
// let's replace f, f', and f" by their analytical expressions in our equation:
// let x = E[X]
// E[f(X)] ~= 2 * (x + b) ^ (1 - p / 2) / (sqrt(a) * (2 - p))
//            + constant / 2 * (1 / sqrt(a) * (-p / 2) * (x + b) ^ (- p / 2 - 1)) * (a * (x + b) ^ p)
//          = 2 * (x + b) ^ (1 - p / 2) / (sqrt(a) * (2 - p))
//            + constant / 2 * 1 / sqrt(a) * (-p / 2) * a * (x + b) ^ (p / 2 - 1)
//          = 2 * (x + b) ^ (1 - p / 2) / (sqrt(a) * (2 - p))
//            - constant / 4 * sqrt(a) * p * (x + b) ^ (p / 2 - 1)
// let z = (x + b) ^ (1 - p / 2)
// E[f(X)] ~= 2 / (sqrt(a) * (2 - p)) * z
//            - constant / 4 * sqrt(a) * p * z^(-1)
// let y = E[f(X)]
// y ~= 2 / (sqrt(a) * (2 - p)) * z - constant / 4 * sqrt(a) * p * z^(-1)
// y * z = 2 / (sqrt(a) * (2 - p)) * z^2 - constant / 4 * sqrt(a) * p
// 0 = 2 / (sqrt(a) * (2 - p)) * z^2 - y * z - constant / 4 * sqrt(a) * p
// let's solve this equation:
// delta = y ^ 2 - 4 * 2 / (sqrt(a) * (2 - p)) * (- constant / 4 * sqrt(a))
//       = y ^ 2 + 2 * p * constant / (2 - p)
// delta >= 0
// the 2 solutions are:
// z0 = (y - sqrt(delta)) / (2 * 2 / (sqrt(a) * (2 - p)))
// z1 = (y + sqrt(delta)) / (2 * 2 / (sqrt(a) * (2 - p)))
// as delta > y^2, sqrt(delta) > y, so z0 is negative
// so z1 is the only possible solution.
// Then, to find E[X], we only have to do:
// z = (x + b) ^ (1 - p / 2) <=> x = z ^ (1 / (1 - p / 2)) - b
//
// What we see here is that a bias compensation term is in delta:
// the term: 2 * p * constant / (2 - p)
// But we are not sure at all what the value of the constant is
// That's why we introduce a user-controled bias parameter to be able to
// control the bias:
// we replace the 2 * p * constant / (2 - p) part of delta by user
// defined bias controller.
static inline void backtransform_v2(float *const buf,
                                    const int wd,
                                    const int ht,
                                    const float a,
                                    const dt_aligned_pixel_t p,
                                    const float b,
                                    const float bias,
                                    const dt_aligned_pixel_t wb)
{
  const size_t npixels = (size_t)wd * ht;
  const dt_aligned_pixel_t expon = { 1.0f / (1.0f - p[0] / 2.0f),
                                     1.0f / (1.0f - p[1] / 2.0f),
                                     1.0f / (1.0f - p[2] / 2.0f),
                                     1.0f };

  const dt_aligned_pixel_t denom = { 4.0f / (sqrtf(a) * (2.0f - p[0])),
                                     4.0f / (sqrtf(a) * (2.0f - p[1])),
                                     4.0f / (sqrtf(a) * (2.0f - p[2])),
                                     1.0f };
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels, buf, b, bias, wb)   \
  dt_omp_sharedconst(expon,denom) \
  schedule(static)
#endif
  for(size_t j = 0; j < 4U * npixels; j += 4)
  {
    for_each_channel(c,aligned(buf,wb))
    {
      const float x = MAX(buf[j+c], 0.0f);
      const float delta = x * x + bias;
      const float z1 = (x + sqrtf(MAX(delta, 0.0f))) / denom[c];
      buf[j+c] = wb[c] * (powf(z1, expon[c]) - b);
    }
  }
}

static inline void precondition_Y0U0V0(const float *const in,
                                       float *const buf,
                                       const int wd,
                                       const int ht,
                                       const float a,
                                       const dt_aligned_pixel_t p,
                                       const float b,
                                       const dt_colormatrix_t toY0U0V0)
{
  const dt_aligned_pixel_t expon = { -p[0] / 2 + 1, -p[1] / 2 + 1, -p[2] / 2 + 1, 1.0f };
  const dt_aligned_pixel_t scale = { 2.0f / ((-p[0] + 2) * sqrtf(a)),
                                     2.0f / ((-p[1] + 2) * sqrtf(a)),
                                     2.0f / ((-p[2] + 2) * sqrtf(a)),
                                     1.0f };
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(buf, ht, in, wd, b, toY0U0V0) \
  dt_omp_sharedconst(expon, scale) \
  schedule(static)
#endif
  for(size_t j = 0; j < (size_t)4 * ht * wd; j += 4)
  {
    dt_aligned_pixel_t tmp; // "unused" fourth element enables vectorization
    for_each_channel(c,aligned(in))
    {
      tmp[c] = powf(MAX(in[j+c] + b, 0.0f), expon[c]) * scale[c];
    }
    for(int c = 0; c < 3; c++)
    {
      float sum = 0.0f;
      for_each_channel(k,aligned(toY0U0V0))
      {
        sum += toY0U0V0[c][k] * tmp[k];
      }
      buf[j+c] = sum;
    }
    buf[j+3] = 0;
  }
}

static inline void backtransform_Y0U0V0(float *const buf,
                                        const int wd,
                                        const int ht,
                                        const float a,
                                        const dt_aligned_pixel_t p,
                                        const float b,
                                        const float bias,
                                        const dt_aligned_pixel_t wb,
                                        const dt_colormatrix_t toRGB)
{
  const dt_aligned_pixel_t bias_wb = { bias * wb[0], bias * wb[1], bias * wb[2], 0.0f };

  const dt_aligned_pixel_t expon = {  1.0f / (1.0f - p[0] / 2.0f),
                                      1.0f / (1.0f - p[1] / 2.0f),
                                      1.0f / (1.0f - p[2] / 2.0f),
                                      1.0f };

  const dt_aligned_pixel_t scale = { (sqrtf(a) * (2.0f - p[0])) / 4.0f,
                                     (sqrtf(a) * (2.0f - p[1])) / 4.0f,
                                     (sqrtf(a) * (2.0f - p[2])) / 4.0f,
                                     1.0f };
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(buf, ht, wd, b, bias_wb, toRGB, expon, scale)  \
  schedule(static)
#endif
  for(size_t j = 0; j < (size_t)4 * ht * wd; j += 4)
  {
    dt_aligned_pixel_t rgb = { 0.0f }; // "unused" fourth element enables vectorization
    for(int k = 0; k < 3; k++)
    {
      for_each_channel(c,aligned(toRGB,buf))
      {
        rgb[k] += toRGB[k][c] * buf[j+c];
      }
    }
    for_each_channel(c,aligned(buf))
    {
      const float x = MAX(rgb[c], 0.0f);
      const float delta = x * x + bias_wb[c];
      const float z1 = (x + sqrtf(MAX(delta, 0.0f))) * scale[c];
      buf[j+c] = powf(z1, expon[c]) - b;
    }
  }
}

// =====================================================================================
// begin common functions
// =====================================================================================

// called by: process_wavelets, nlmeans_precondition,
//     nlmeans_precondition_cl, process_variance, process_wavelets_cl
static void compute_wb_factors(dt_aligned_pixel_t wb,
                               const dt_iop_denoiseprofile_data_t *const d,
                               const dt_dev_pixelpipe_iop_t *const piece,
                               const dt_aligned_pixel_t weights)
{
  const float wb_mean =
    (piece->pipe->dsc.temperature.coeffs[0] + piece->pipe->dsc.temperature.coeffs[1]
     + piece->pipe->dsc.temperature.coeffs[2])
    / 3.0f;
  // we init wb by the mean of the coeffs, which corresponds to the mean
  // amplification that is done in addition to the "ISO" related amplification
  wb[0] = wb[1] = wb[2] = wb[3] = wb_mean;

  if(d->fix_anscombe_and_nlmeans_norm)
  {
    if(wb_mean != 0.0f && d->wb_adaptive_anscombe)
    {
      for(int i = 0; i < 3; i++) wb[i] = piece->pipe->dsc.temperature.coeffs[i];
    }
    else if(wb_mean == 0.0f)
    {
      // temperature coeffs are equal to 0 if we open a JPG image.
      // in this case consider them equal to 1.
      for_each_channel(i)
        wb[i] = 1.0f;
    }
    // else, wb_adaptive_anscombe is false and our wb array is
    // filled with the wb_mean
  }
  else
  {
    for_each_channel(i)
      wb[i] = weights[i] * piece->pipe->dsc.processed_maximum[i];
  }
  return;
}

// =====================================================================================

static gboolean invert_matrix(const dt_colormatrix_t in,
                              dt_colormatrix_t out)
{
  // use same notation as https://en.wikipedia.org/wiki/Invertible_matrix#Inversion_of_3_%C3%97_3_matrices
  const float biga = in[1][1] * in[2][2] - in[1][2] * in[2][1];
  const float bigb = -in[1][0] * in[2][2] + in[1][2] * in[2][0];
  const float bigc = in[1][0] * in[2][1] - in[1][1] * in[2][0];
  const float bigd = -in[0][1] * in[2][2] + in[0][2] * in[2][1];
  const float bige = in[0][0] * in[2][2] - in[0][2] * in[2][0];
  const float bigf = -in[0][0] * in[2][1] + in[0][1] * in[2][0];
  const float bigg = in[0][1] * in[1][2] - in[0][2] * in[1][1];
  const float bigh = -in[0][0] * in[1][2] + in[0][2] * in[1][0];
  const float bigi = in[0][0] * in[1][1] - in[0][1] * in[1][0];

  const float det = in[0][0] * biga + in[0][1] * bigb + in[0][2] * bigc;
  if(det == 0.0f)
  {
    return FALSE;
  }

  out[0][0] = 1.0f / det * biga;
  out[0][1] = 1.0f / det * bigd;
  out[0][2] = 1.0f / det * bigg;
  out[0][3] = 0.0f;
  out[1][0] = 1.0f / det * bigb;
  out[1][1] = 1.0f / det * bige;
  out[1][2] = 1.0f / det * bigh;
  out[1][3] = 0.0f;
  out[2][0] = 1.0f / det * bigc;
  out[2][1] = 1.0f / det * bigf;
  out[2][2] = 1.0f / det * bigi;
  out[2][3] = 0.0f;
  return TRUE;
}

// create the white balance adaptative conversion matrices
// supposes toY0U0V0 already contains the "normal" conversion matrix
static void set_up_conversion_matrices(dt_colormatrix_t toY0U0V0,
                                       dt_colormatrix_t toRGB,
                                       const dt_aligned_pixel_t wb)
{
  // for an explanation of the spirit of the choice of the coefficients of the
  // Y0U0V0 conversion matrix, see part 12.3.3 page 190 of
  // "From Theory to Practice, a Tour of Image Denoising"
  // https://hal.archives-ouvertes.fr/tel-01114299
  // we adapt a bit the coefficients, in a way that follows the same spirit.

  float sum_invwb = 1.0f/wb[0] + 1.0f/wb[1] + 1.0f/wb[2];
  // we change the coefs to Y0, but keeping the goal of making SNR higher:
  // these were all equal to 1/3 to get the Y0 the least noisy possible, assuming
  // that all channels have equal noise variance.
  // as white balance influences noise variance, we do a weighted mean depending
  // on white balance. Note that it is equivalent to keeping the 1/3 coefficients
  // if we divide by the white balance coefficients beforehand.
  // we then normalize the line so that variance becomes equal to 1:
  // var(Y0) = 1/9 * (var(R) + var(G) + var(B)) = 1/3
  // var(sqrt(3)Y0) = 1
  sum_invwb *= sqrtf(3);
  toY0U0V0[0][0] = sum_invwb / wb[0];
  toY0U0V0[0][1] = sum_invwb / wb[1];
  toY0U0V0[0][2] = sum_invwb / wb[2];
  toY0U0V0[0][3] = 0.0f;
  // we also normalize the other line in a way that should give a variance of 1
  // if var(B/wb[B]) == 1, then var(B) = wb[B]^2
  // note that we don't change the coefs of U0 and V0 depending on white balance,
  // apart of the normalization: these coefficients do differences of RGB channels
  // to try to reduce or cancel the signal. If we change these depending on white
  // balance, we will not reduce/cancel the signal anymore.
  const float stddevU0 = sqrtf(0.5f * 0.5f * wb[0] * wb[0]
                               + 0.5f * 0.5f * wb[2] * wb[2]);
  const float stddevV0 = sqrtf(0.25f * 0.25f * wb[0] * wb[0]
                               + 0.5f * 0.5f * wb[1] * wb[1]
                               + 0.25f * 0.25f * wb[2] * wb[2]);
  toY0U0V0[1][0] /= stddevU0;
  toY0U0V0[1][1] /= stddevU0;
  toY0U0V0[1][2] /= stddevU0;
  toY0U0V0[1][3] = 0.0f;
  toY0U0V0[2][0] /= stddevV0;
  toY0U0V0[2][1] /= stddevV0;
  toY0U0V0[2][2] /= stddevV0;
  toY0U0V0[2][3] = 0.0f;
  const gboolean is_invertible = invert_matrix(toY0U0V0, toRGB);
  if(!is_invertible)
  {
    // use standard form if whitebalance adapted matrix is not invertible
    float stddevY0 = sqrtf(1.0f / 9.0f * (wb[0] * wb[0] + wb[1] * wb[1] + wb[2] * wb[2]));
    toY0U0V0[0][0] = 1.0f / (3.0f * stddevY0);
    toY0U0V0[0][1] = 1.0f / (3.0f * stddevY0);
    toY0U0V0[0][2] = 1.0f / (3.0f * stddevY0);
    toY0U0V0[0][3] = 0.0f;
    invert_matrix(toY0U0V0, toRGB);
  }
}

static void variance_stabilizing_xform(dt_aligned_pixel_t thrs,
                                       const int scale,
                                       const int max_scale,
                                       const size_t npixels,
                                       const float *const sum_y2,
                                       const dt_iop_denoiseprofile_data_t *const d)
{
  // variance stabilizing transform maps sigma to unity.
  const float sigma = 1.0f;
  // it is then transformed by wavelet scales via the 5 tap a-trous filter:
  const float varf = sqrtf(2.0f + 2.0f * 4.0f * 4.0f + 6.0f * 6.0f) / 16.0f; // about 0.5
  const float sigma_band = powf(varf, scale) * sigma;
  // determine thrs as bayesshrink
  const float sb2 = sigma_band * sigma_band;

  const dt_aligned_pixel_t var_y =
    { sum_y2[0] / (npixels - 1.0f),
      sum_y2[1] / (npixels - 1.0f),
      sum_y2[2] / (npixels - 1.0f),
      0.0f };

  const dt_aligned_pixel_t std_x =
    { sqrtf(MAX(1e-6f, var_y[0] - sb2)),
      sqrtf(MAX(1e-6f, var_y[1] - sb2)),
      sqrtf(MAX(1e-6f, var_y[2] - sb2)),
      1.0f };

  // add 8.0 here because it seemed a little weak
  dt_aligned_pixel_t adjt = { 8.0f, 8.0f, 8.0f, 0.0f };

  const int offset_scale = DT_IOP_DENOISE_PROFILE_BANDS - max_scale;
  const int band_index = DT_IOP_DENOISE_PROFILE_BANDS - (scale + offset_scale + 1);

  if(d->wavelet_color_mode == MODE_RGB)
  {
    // current scale number is scale+offset_scale
    // for instance, largest scale is DT_IOP_DENOISE_PROFILE_BANDS
    // max_scale only indicates the number of scales to process at THIS
    // zoom level, it does NOT corresponds to the the maximum number of scales.
    // in other words, max_scale is the maximum number of VISIBLE scales.
    // That is why we have this "scale+offset_scale"
    float band_force_exp_2
      = d->force[DT_DENOISE_PROFILE_ALL][band_index];
    band_force_exp_2 *= band_force_exp_2;
    band_force_exp_2 *= 4;
    for_each_channel(ch)
    {
      adjt[ch] *= band_force_exp_2;
    }
    band_force_exp_2 = d->force[DT_DENOISE_PROFILE_R][band_index];
    band_force_exp_2 *= band_force_exp_2;
    band_force_exp_2 *= 4;
    adjt[0] *= band_force_exp_2;
    band_force_exp_2 = d->force[DT_DENOISE_PROFILE_G][band_index];
    band_force_exp_2 *= band_force_exp_2;
    band_force_exp_2 *= 4;
    adjt[1] *= band_force_exp_2;
    band_force_exp_2 = d->force[DT_DENOISE_PROFILE_B][band_index];
    band_force_exp_2 *= band_force_exp_2;
    band_force_exp_2 *= 4;
    adjt[2] *= band_force_exp_2;
  }
  else
  {
    float band_force_exp_2 = d->force[DT_DENOISE_PROFILE_Y0][band_index];
    band_force_exp_2 *= band_force_exp_2;
    band_force_exp_2 *= 4;
    adjt[0] *= band_force_exp_2;
    band_force_exp_2 = d->force[DT_DENOISE_PROFILE_U0V0][band_index];
    band_force_exp_2 *= band_force_exp_2;
    band_force_exp_2 *= 4;
    adjt[1] *= band_force_exp_2;
    adjt[2] *= band_force_exp_2;
  }
  for_each_channel(c)
    thrs[c] = adjt[c] * sb2 / std_x[c];
}

static void process_wavelets(struct dt_iop_module_t *self,
                             dt_dev_pixelpipe_iop_t *piece,
                             const void *const ivoid,
                             void *const ovoid,
                             const dt_iop_roi_t *const roi_in,
                             const dt_iop_roi_t *const roi_out,
                             const eaw_dn_decompose_t decompose,
                             const eaw_synthesize_t synthesize)
{
  // this is called for preview and full pipe separately, each with
  // its own pixelpipe piece.  get our data struct:
  const dt_iop_denoiseprofile_data_t *const d =
    (dt_iop_denoiseprofile_data_t *)piece->data;

#define MAX_MAX_SCALE DT_IOP_DENOISE_PROFILE_BANDS // hard limit

  int max_scale = 0;
  const float in_scale = fminf(roi_in->scale / piece->iscale, 1.0f);
  // largest desired filter on input buffer (20% of input dim)
  const float supp0 = MIN(2 * (2u << (MAX_MAX_SCALE - 1)) + 1,
                          MAX(piece->buf_in.height * piece->iscale,
                              piece->buf_in.width * piece->iscale) * 0.2f);
  const float i0 = dt_log2f((supp0 - 1.0f) * .5f);

  for(; max_scale < MAX_MAX_SCALE; max_scale++)
  {
    // actual filter support on scaled buffer
    const float supp = 2 * (2u << max_scale) + 1;
    // approximates this filter size on unscaled input image:
    const float supp_in = supp * (1.0f / in_scale);
    const float i_in = dt_log2f((supp_in - 1) * .5f) - 1.0f;
    // i_in = max_scale .. .. .. 0
    const float t = 1.0f - (i_in + .5f) / i0;
    if(t < 0.0f) break;
  }

  const int max_mult = 1u << (max_scale - 1);
  const int width = roi_in->width, height = roi_in->height;
  const size_t npixels = (size_t)width*height;
  const float *const restrict in = (const float*)ivoid;
  float *const restrict out = (float*)ovoid;

  // corner case of extremely small image. this is not really likely
  // to happen but would lead to out of bounds memory access
  if(width < 2 * max_mult || height < 2 * max_mult)
  {
    memcpy(out, in, sizeof(float) * 4 * npixels);
    return;
  }

  float *buf = NULL;
  float *restrict precond = NULL;
  float *restrict tmp = NULL;

  if(!dt_iop_alloc_image_buffers(self, roi_in, roi_out, 4, &precond, 4, &tmp, 4, &buf, 0))
  {
    dt_iop_copy_image_roi(out, in, piece->colors, roi_in, roi_out, TRUE);
    return;
  }

  dt_aligned_pixel_t wb;  // the "unused" fourth element enables vectorization
  const dt_aligned_pixel_t wb_weights = { 2.0f, 1.0f, 2.0f, 0.0f };
  compute_wb_factors(wb,d,piece,wb_weights);

  // adaptive p depending on white balance (the "unused" fourth
  // element enables vectorization
  const dt_aligned_pixel_t p = { MAX(d->shadows + 0.1 * logf(in_scale / wb[0]), 0.0f),
                                 MAX(d->shadows + 0.1 * logf(in_scale / wb[1]), 0.0f),
                                 MAX(d->shadows + 0.1 * logf(in_scale / wb[2]), 0.0f),
                                 0.0f };

  const float compensate_p =
    DT_IOP_DENOISE_PROFILE_P_FULCRUM / powf(DT_IOP_DENOISE_PROFILE_P_FULCRUM, d->shadows);

  // conversion to Y0U0V0 space as defined in Secrets of image denoising cuisine
  dt_colormatrix_t toY0U0V0 = { { 1.0f/3.0f, 1.0f/3.0f, 1.0f/3.0f },
                                { 0.5f,      0.0f,      -0.5f },
                                {  0.25f,     -0.5f,     0.25f } };

  // "unused" fourth element enables vectorization:
  dt_colormatrix_t toRGB = { { 0.0f, 0.0f, 0.0f },
                             { 0.0f, 0.0f, 0.0f },
                             { 0.0f, 0.0f, 0.0f } };
  set_up_conversion_matrices(toY0U0V0, toRGB, wb);

  // more strength in Y0U0V0 in order to get a similar smoothing as in other modes
  // otherwise, result was much less denoised in Y0U0V0 mode.
  const float compensate_strength = (d->wavelet_color_mode == MODE_RGB) ? 1.0f : 2.5f;
  // update the coeffs with strength and scale
  for(size_t k = 0; k < 3; k++)
    for_each_channel(c)
    {
      toY0U0V0[k][c] /= (d->strength * compensate_strength * in_scale);
      toRGB[k][c] *= (d->strength * compensate_strength * in_scale);
    }
  for_each_channel(i)
    wb[i] *= d->strength * compensate_strength * in_scale;

  // only use green channel + wb for now: (the "unused" fourth element
  // enables vectorization)
  const dt_aligned_pixel_t aa = { d->a[1] * wb[0], d->a[1] * wb[1], d->a[1] * wb[2], 0.0f };
  const dt_aligned_pixel_t bb = { d->b[1] * wb[0], d->b[1] * wb[1], d->b[1] * wb[2], 0.0f };

  if(!d->use_new_vst)
  {
    precondition(in, precond, width, height, aa, bb);
  }
  else if(d->wavelet_color_mode == MODE_RGB)
  {
    precondition_v2(in, precond, width, height, d->a[1] * compensate_p, p, d->b[1], wb);
  }
  else
  {
    precondition_Y0U0V0(in, precond, width, height,
                        d->a[1] * compensate_p,
                        p, d->b[1],
                        toY0U0V0);
  }

  debug_dump_PFM(piece, "transformed", precond, width, height, 0);

  float *restrict buf1 = precond;
  float *restrict buf2 = tmp;

  // clear the output buffer, which will be accumulating all of the detail scales
  dt_iop_image_fill(out, 0.0f, width, height, 4);

  for(int scale = 0; scale < max_scale; scale++)
  {
    const float sigma = 1.0f;
    const float varf = sqrtf(2.0f + 2.0f * 4.0f * 4.0f + 6.0f * 6.0f) / 16.0f; // about 0.5
    const float sigma_band = powf(varf, scale) * sigma;
    dt_aligned_pixel_t sum_y2;
    decompose(buf2, buf1, buf, sum_y2,
              scale, 1.0f / (sigma_band * sigma_band), width, height);
    debug_dump_PFM(piece, "coarse_%d", buf2, width, height, scale);
    debug_dump_PFM(piece, "detail_%d", buf, width, height, scale);

    const dt_aligned_pixel_t boost = { 1.0f, 1.0f, 1.0f, 1.0f };
    dt_aligned_pixel_t thrs;
    variance_stabilizing_xform(thrs, scale, max_scale, npixels, sum_y2, d);
    synthesize(out, out, buf, thrs, boost, width, height);

    float *buf3 = buf2;
    buf2 = buf1;
    buf1 = buf3;
  }

  // add in the final residue
#ifdef _OPENMP
#pragma omp simd aligned(buf1, out : 64)
#endif
  for(size_t k = 0; k < 4U * npixels; k++)
    out[k] += buf1[k];

  if(!d->use_new_vst)
  {
    backtransform(out, width, height, aa, bb);
  }
  else if(d->wavelet_color_mode == MODE_RGB)
  {
    backtransform_v2(out, width, height, d->a[1] * compensate_p,
                     p, d->b[1], d->bias - 0.5 * logf(in_scale), wb);
  }
  else
  {
    backtransform_Y0U0V0(out, width, height, d->a[1] * compensate_p,
                         p, d->b[1], d->bias - 0.5 * logf(in_scale), wb, toRGB);
  }

  dt_free_align(buf);
  dt_free_align(tmp);
  dt_free_align(precond);

#undef MAX_MAX_SCALE
}

#if defined(HAVE_OPENCL) && !USE_NEW_IMPL_CL
static int sign(int a)
{
  return (a > 0) - (a < 0);
}
#endif

// called by: process_nlmeans_cpu, process_nlmeans_cl
static float nlmeans_norm(const int P,
                          const dt_iop_denoiseprofile_data_t *const d)
{
  // Each patch has a width of 2P+1 and a height of 2P+1
  // thus, divide by (2P+1)^2.
  // The 0.045 was derived from the old formula, to keep the
  // norm identical when P=1, as the norm for P=1 seemed
  // to work quite well: 0.045 = 0.015 * (2 * P + 1) with P=1.
  float norm = .045f / ((2 * P + 1) * (2 * P + 1));
  if(!d->fix_anscombe_and_nlmeans_norm)
  {
    // use old formula
    norm = .015f / (2 * P + 1);
  }
  return norm;
}

// adjust the user-specified scattering factor and search radius to
// account for the type of pixelpipe called by: process_nlmeans_cpu,
// process_nlmeans_cl
static float nlmeans_scattering(int *nbhood,
                                const dt_iop_denoiseprofile_data_t *const d,
                                const dt_dev_pixelpipe_iop_t *const piece,
                                const float scale)
{
  int K = *nbhood;
  float scattering = d->scattering;

  if(piece->pipe->type
     & (DT_DEV_PIXELPIPE_PREVIEW | DT_DEV_PIXELPIPE_PREVIEW2 | DT_DEV_PIXELPIPE_THUMBNAIL))
  {
    // much faster slightly more inaccurate preview
    const int maxk = (K * K * K + 7.0 * K * sqrt(K)) * scattering / 6.0 + K;
    K = MIN(3, K);
    scattering = (maxk - K) * 6.0 / (K * K * K + 7.0 * K * sqrt(K));
  }
  if(piece->pipe->type & DT_DEV_PIXELPIPE_FULL)
  {
    // much faster slightly more inaccurate preview
    const int maxk = (K * K * K + 7.0 * K * sqrt(K)) * scattering / 6.0 + K;
    K = MAX(MIN(4, K), K * scale);
    scattering = (maxk - K) * 6.0 / (K * K * K + 7.0 * K * sqrt(K));
  }
  *nbhood = K;
  return scattering;
}

// called by process_nlmeans_cpu
// must keep synchronized with nlmeans_precondition_cl below
static float nlmeans_precondition(const dt_iop_denoiseprofile_data_t *const d,
                                  const dt_dev_pixelpipe_iop_t *const piece,
                                  dt_aligned_pixel_t wb,
                                  const void *const ivoid,
                                  const dt_iop_roi_t *const roi_in,
                                  float scale, float *in,
                                  dt_aligned_pixel_t aa,
                                  dt_aligned_pixel_t bb,
                                  dt_aligned_pixel_t p)
{
  // the "unused" fourth array element enables vectorization
  const dt_aligned_pixel_t wb_weights = { 1.0f, 1.0f, 1.0f, 0.0f };
  compute_wb_factors(wb,d,piece,wb_weights);

  // adaptive p depending on white balance
  p[0] = MAX(d->shadows + 0.1 * logf(scale / wb[0]), 0.0f);
  p[1] = MAX(d->shadows + 0.1 * logf(scale / wb[1]), 0.0f);
  p[2] = MAX(d->shadows + 0.1 * logf(scale / wb[2]), 0.0f);
  p[3] = 0.0f;

  // update the coeffs with strength and scale
  for_each_channel(i,aligned(wb,aa,bb))
  {
    wb[i] *= d->strength * scale;
    // only use green channel + wb for now:
    aa[i] = d->a[1] * wb[i];
    bb[i] = d->b[1] * wb[i];
  }
  const float compensate_p =
    DT_IOP_DENOISE_PROFILE_P_FULCRUM / powf(DT_IOP_DENOISE_PROFILE_P_FULCRUM, d->shadows);
  if(!d->use_new_vst)
  {
    precondition((float *)ivoid, in, roi_in->width, roi_in->height, aa, bb);
  }
  else
  {
    precondition_v2((float *)ivoid, in, roi_in->width, roi_in->height,
                    d->a[1] * compensate_p, p, d->b[1], wb);
  }
  return compensate_p;
}

#ifdef HAVE_OPENCL
// called by process_nlmeans_cl
// must keep synchronized with nlmeans_precondition above
static float nlmeans_precondition_cl(const dt_iop_denoiseprofile_data_t *const d,
                                     const dt_dev_pixelpipe_iop_t *const piece,
                                     dt_aligned_pixel_t wb,
                                     float scale, dt_aligned_pixel_t aa,
                                     dt_aligned_pixel_t bb,
                                     dt_aligned_pixel_t p)
{
  // the "unused" fourth element enables vectorization
  const dt_aligned_pixel_t wb_weights = { 1.0f, 1.0f, 1.0f, 0.0f };
  compute_wb_factors(wb,d,piece,wb_weights);
  wb[3] = 0.0;

  // adaptive p depending on white balance
  p[0] = MAX(d->shadows + 0.1 * logf(scale / wb[0]), 0.0f);
  p[1] = MAX(d->shadows + 0.1 * logf(scale / wb[1]), 0.0f);
  p[2] = MAX(d->shadows + 0.1 * logf(scale / wb[2]), 0.0f);
  p[3] = 1.0f;

  // update the coeffs with strength and scale
  for_each_channel(i,aligned(wb,aa,bb))
  {
    wb[i] *= d->strength * scale;
    // only use green channel + wb for now:
    aa[i] = d->a[1] * wb[i];
    bb[i] = d->b[1] * wb[i];
  }
  aa[3] = 1.0f;
  bb[3] = 1.0f;
  const float compensate_p =
    DT_IOP_DENOISE_PROFILE_P_FULCRUM / powf(DT_IOP_DENOISE_PROFILE_P_FULCRUM, d->shadows);
  if(d->use_new_vst)
  {
    for_each_channel(c,aligned(aa,bb))
    {
      aa[c] = d->a[1] * compensate_p;
      bb[c] = d->b[1];
    }
  }
  return compensate_p;
}
#endif /* HAVE_OPENCL */

// called by process_nlmeans_cpu
static void nlmeans_backtransform(const dt_iop_denoiseprofile_data_t *const d,
                                  float *ovoid,
                                  const dt_iop_roi_t *const roi_in,
                                  const float scale,
                                  const float compensate_p,
                                  const dt_aligned_pixel_t wb,
                                  const dt_aligned_pixel_t aa,
                                  const dt_aligned_pixel_t bb,
                                  const dt_aligned_pixel_t p)
{
  if(!d->use_new_vst)
  {
    backtransform((float *)ovoid, roi_in->width, roi_in->height, aa, bb);
  }
  else
  {
    backtransform_v2((float *)ovoid, roi_in->width, roi_in->height,
                     d->a[1] * compensate_p, p, d->b[1],
                     d->bias - 0.5 * logf(scale), wb);
  }
  return;
}

static void process_nlmeans(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        const void *const ivoid,
        void *const ovoid,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  const dt_iop_denoiseprofile_data_t *const d =
    (dt_iop_denoiseprofile_data_t *)piece->data;
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/,
                                        piece->module, piece->colors,
                                        ivoid, ovoid, roi_in, roi_out))
    return; // image has been copied through to output and module's
            // trouble flag has been updated

  float *restrict in;
  if(!dt_iop_alloc_image_buffers(piece->module, roi_in, roi_out,
                                 4 | DT_IMGSZ_INPUT, &in, 0))
    return;

  // adjust to zoom size:
  const float scale = fminf(fminf(roi_in->scale, 2.0f) / fmaxf(piece->iscale, 1.0f), 1.0f);
  const int P = ceilf(d->radius * scale); // pixel filter size
  int K = d->nbhood; // nbhood
  const float scattering = nlmeans_scattering(&K,d,piece,scale);
  const float norm = nlmeans_norm(P,d);
  const float central_pixel_weight = d->central_pixel_weight * scale;

  // P == 0 : this will degenerate to a (fast) bilateral filter.

  dt_aligned_pixel_t wb; // the "unused" fourth array element enables vectorization
  dt_aligned_pixel_t p;
  dt_aligned_pixel_t aa, bb;
  const float compensate_p =
    nlmeans_precondition(d, piece, wb, ivoid, roi_in, scale, in, aa, bb, p);

  const dt_aligned_pixel_t norm2 = { 1.0f, 1.0f, 1.0f, 1.0f };
  const dt_nlmeans_param_t params = { .scattering = scattering,
                                      .scale = scale,
                                      .luma = 1.0,    //no blending
                                      .chroma = 1.0,
                                      .center_weight = central_pixel_weight,
                                      .sharpness = norm,
                                      .patch_radius = P,
                                      .search_radius = K,
                                      .decimate = 0,
                                      .norm = norm2 };
  nlmeans_denoise(in, ovoid, roi_in, roi_out, &params);

  dt_free_align(in);
  nlmeans_backtransform(d,ovoid,roi_in,scale,compensate_p,wb,aa,bb,p);
}

static void sum_rec(const size_t npixels,
                    const float *in,
                    float *out)
{
  if(npixels <= 3)
  {
    for_each_channel(c,aligned(out))
    {
      out[c] = 0.0;
    }
    for(size_t i = 0; i < npixels; i++)
    {
      for_each_channel(c,aligned(in,out))
      {
        out[c] += in[i * 4 + c];
      }
    }
    return;
  }

  const size_t npixels_first_half = npixels >> 1;
  const size_t npixels_second_half = npixels - npixels_first_half;
  sum_rec(npixels_first_half, in, out);
  sum_rec(npixels_second_half, in + 4U * npixels_first_half, out + 4U * npixels_first_half);
  for_each_channel(c,aligned(out))
  {
    out[c] += out[4U * npixels_first_half + c];
  }
}

/* this gives (npixels-1)*V[X] */
static void variance_rec(const size_t npixels,
                         const float *in,
                         float *out, const
                         dt_aligned_pixel_t mean)
{
  if(npixels <= 3)
  {
    for_each_channel(c,aligned(out))
    {
      out[c] = 0.0;
    }
    for(size_t i = 0; i < npixels; i++)
    {
      for_each_channel(c,aligned(in,out))
      {
        const float diff = in[i * 4 + c] - mean[c];
        out[c] += diff * diff;
      }
    }
    return;
  }

  const size_t npixels_first_half = npixels >> 1;
  const size_t npixels_second_half = npixels - npixels_first_half;
  variance_rec(npixels_first_half, in, out, mean);
  variance_rec(npixels_second_half, in + 4U * npixels_first_half,
               out + 4U * npixels_first_half, mean);
  for_each_channel(c,aligned(out))
  {
    out[c] += out[4U * npixels_first_half + c];
  }
}

static void process_variance(struct dt_iop_module_t *self,
                             dt_dev_pixelpipe_iop_t *piece,
                             const void *const ivoid,
                             void *const ovoid,
                             const dt_iop_roi_t *const roi_in,
                             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_denoiseprofile_data_t *const d = piece->data;
  dt_iop_denoiseprofile_gui_data_t *g = (dt_iop_denoiseprofile_gui_data_t*)self->gui_data;

  const int width = roi_in->width, height = roi_in->height;
  size_t npixels = (size_t)width * height;

  dt_iop_image_copy_by_size(ovoid, ivoid, width, height, 4);
  if((piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW) || (g == NULL))
  {
    return;
  }

  float *restrict in;
  if(!dt_iop_alloc_image_buffers(self, roi_in, roi_out, 4 | DT_IMGSZ_INPUT, &in, 0))
    return;

  dt_aligned_pixel_t wb;  // the "unused" fourth element enables vectorization
  const dt_aligned_pixel_t wb_weights = { 1.0f, 1.0f, 1.0f, 0.0f };
  compute_wb_factors(wb,d,piece,wb_weights);

  // adaptive p depending on white balance
  const dt_aligned_pixel_t p = { MAX(d->shadows - 0.1 * logf(wb[0]), 0.0f),
                                 MAX(d->shadows - 0.1 * logf(wb[1]), 0.0f),
                                 MAX(d->shadows - 0.1 * logf(wb[2]), 0.0f),
                                 0.0f };

  // update the coeffs with strength
  for_each_channel(i) wb[i] *= d->strength;

  const float compensate_p =
    DT_IOP_DENOISE_PROFILE_P_FULCRUM / powf(DT_IOP_DENOISE_PROFILE_P_FULCRUM, d->shadows);
  precondition_v2((float *)ivoid, (float *)ovoid, roi_in->width, roi_in->height,
                  d->a[1] * compensate_p, p, d->b[1], wb);

  float *out = (float *)ovoid;
  // we use out as a temporary buffer here
  // compute mean
  sum_rec(npixels, in, out);
  dt_aligned_pixel_t mean; // the "unused" fourth array element enables vectorization
  for_each_channel(c,aligned(out))
  {
    mean[c] = out[c] / npixels;
  }
  variance_rec(npixels, in, out, mean);
  dt_aligned_pixel_t var; // the "unused" fourth array element enables vectorization
  for_each_channel(c,aligned(out))
  {
    var[c] = out[c] / (npixels - 1);
  }
  g->variance_R = var[0];
  g->variance_G = var[1];
  g->variance_B = var[2];

  dt_iop_image_copy_by_size(ovoid, ivoid, width, height, 4);
}

#if defined(HAVE_OPENCL) && !USE_NEW_IMPL_CL
static int bucket_next(unsigned int *state, unsigned int max)
{
  const unsigned int current = *state;
  const unsigned int next = (current >= max - 1 ? 0 : current + 1);

  *state = next;

  return next;
}
#endif

#if defined(HAVE_OPENCL)
static int process_nlmeans_cl(struct dt_iop_module_t *self,
                              dt_dev_pixelpipe_iop_t *piece,
                              cl_mem dev_in,
                              cl_mem dev_out,
                              const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out)
{
  dt_iop_denoiseprofile_data_t *d = (dt_iop_denoiseprofile_data_t *)piece->data;
  dt_iop_denoiseprofile_global_data_t *gd =
    (dt_iop_denoiseprofile_global_data_t *)self->global_data;
#if USE_NEW_IMPL_CL
  const int width = roi_in->width;
  const int height = roi_in->height;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const float scale = fminf(fminf(roi_in->scale, 2.0f) / fmaxf(piece->iscale, 1.0f), 1.0f);
  const int P = ceilf(d->radius * scale); // pixel filter size
  int K = d->nbhood; // nbhood
  const float scattering = nlmeans_scattering(&K,d,piece,scale);
  const float norm = nlmeans_norm(P,d);
  const float central_pixel_weight = d->central_pixel_weight * scale;

  dt_aligned_pixel_t wb;
  dt_aligned_pixel_t p;
  dt_aligned_pixel_t aa;
  dt_aligned_pixel_t bb;
  (void)nlmeans_precondition_cl(d,piece,wb,scale,aa,bb,p);

  // allocate a buffer for a preconditioned copy of the image
  const int devid = piece->pipe->devid;
  cl_mem dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(dev_tmp == NULL)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_denoiseprofile] couldn't allocate GPU buffer\n");
    return FALSE;
  }

  const size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  const float sigma2[4] = { (bb[0] / aa[0]) * (bb[0] / aa[0]),
                            (bb[1] / aa[1]) * (bb[1] / aa[1]),
                            (bb[2] / aa[2]) * (bb[2] / aa[2]),
                            0.0f };

  if(!d->use_new_vst)
  {
    dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_precondition,
                              0, CLARG(dev_in), CLARG(dev_tmp),
      CLARG(width), CLARG(height), CLARG(aa), CLARG(sigma2));
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_precondition, sizes);
  }
  else
  {
    dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_precondition_v2,
                              0, CLARG(dev_in), CLARG(dev_tmp),
      CLARG(width), CLARG(height), CLARG(aa), CLARG(p), CLARG(bb), CLARG(wb));
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_precondition_v2,
                                      sizes);
  }

  // allocate a buffer to receive the denoised image
  cl_mem dev_U2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * width * height);
  if(dev_U2 == NULL) err = DT_OPENCL_DEFAULT_ERROR;

  if(err == CL_SUCCESS)
  {
    const dt_aligned_pixel_t norm2 = { 1.0f, 1.0f, 1.0f, 1.0f };
    const dt_nlmeans_param_t params =
      {
        .scattering = scattering,
        .scale = scale,
        .luma = 1.0f,
        .chroma = 1.0f,
        .center_weight = central_pixel_weight,
        .sharpness = norm,
        .patch_radius = P,
        .search_radius = K,
        .decimate = 0,
        .norm = norm2,
        .pipetype = piece->pipe->type,
        .kernel_init = gd->kernel_denoiseprofile_init,
        .kernel_dist = gd->kernel_denoiseprofile_dist,
        .kernel_horiz = gd->kernel_denoiseprofile_horiz,
        .kernel_vert = gd->kernel_denoiseprofile_vert,
        .kernel_accu = gd->kernel_denoiseprofile_accu
      };
    err = nlmeans_denoiseprofile_cl(&params, devid, dev_tmp, dev_U2, roi_in);
  }
  if(err == CL_SUCCESS)
  {
    if(!d->use_new_vst)
    {
      dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_finish,
                                0, CLARG(dev_in), CLARG(dev_U2),
        CLARG(dev_out), CLARG(width), CLARG(height), CLARG(aa), CLARG(sigma2));
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_finish, sizes);
    }
    else
    {
      const float bias = d->bias - 0.5 * logf(scale);
      dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_finish_v2, 0,
                                CLARG(dev_in), CLARG(dev_U2),
                                CLARG(dev_out), CLARG(width), CLARG(height),
                                CLARG(aa), CLARG(p),
                                CLARG(bb), CLARG(bias), CLARG(wb));
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_finish_v2, sizes);
    }
  }
  dt_opencl_release_mem_object(dev_U2);
  dt_opencl_release_mem_object(dev_tmp);
  if(err == CL_SUCCESS)
    return TRUE;
  dt_print(DT_DEBUG_OPENCL,
           "[opencl_denoiseprofile] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;

#else
  const int width = roi_in->width;
  const int height = roi_in->height;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const float scale = fminf(fminf(roi_in->scale, 2.0f) / fmaxf(piece->iscale, 1.0f), 1.0f);
  const int P = ceilf(d->radius * scale); // pixel filter size
  int K = d->nbhood; // nbhood
  const float scattering = nlmeans_scattering(&K,d,piece,scale);
  const float norm = nlmeans_norm(P,d);
  const float central_pixel_weight = d->central_pixel_weight * scale;

  dt_aligned_pixel_t wb;
  dt_aligned_pixel_t p;
  dt_aligned_pixel_t aa;
  dt_aligned_pixel_t bb;
  (void)nlmeans_precondition_cl(d,piece,wb,scale,aa,bb,p);

  const dt_aligned_pixel_t sigma2 = { (bb[0] / aa[0]) * (bb[0] / aa[0]),
                                      (bb[1] / aa[1]) * (bb[1] / aa[1]),
                                      (bb[2] / aa[2]) * (bb[2] / aa[2]),
                                      0.0f };

  const int devid = piece->pipe->devid;
  cl_mem dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(dev_tmp == NULL) goto error;

  cl_mem dev_U2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * width * height);
  if(dev_U2 == NULL) goto error;

  cl_mem buckets[NUM_BUCKETS] = { NULL };
  unsigned int state = 0;
  for(int k = 0; k < NUM_BUCKETS; k++)
  {
    buckets[k] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * width * height);
    if(buckets[k] == NULL) goto error;
  }

  int hblocksize;
  dt_opencl_local_buffer_t hlocopt
    = (dt_opencl_local_buffer_t){ .xoffset = 2 * P,
                                  .xfactor = 1,
                                  .yoffset = 0,
                                  .yfactor = 1,
                                  .cellsize = sizeof(float),
                                  .overhead = 0,
                                  .sizex = 1u << 16,
                                  .sizey = 1 };

  if(dt_opencl_local_buffer_opt(devid, gd->kernel_denoiseprofile_horiz, &hlocopt))
    hblocksize = hlocopt.sizex;
  else
    hblocksize = 1;

  int vblocksize;
  dt_opencl_local_buffer_t vlocopt
    = (dt_opencl_local_buffer_t){ .xoffset = 1,
                                  .xfactor = 1,
                                  .yoffset = 2 * P,
                                  .yfactor = 1,
                                  .cellsize = sizeof(float),
                                  .overhead = 0,
                                  .sizex = 1,
                                  .sizey = 1u << 16 };

  if(dt_opencl_local_buffer_opt(devid, gd->kernel_denoiseprofile_vert, &vlocopt))
    vblocksize = vlocopt.sizey;
  else
    vblocksize = 1;


  const size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  size_t sizesl[3];
  size_t local[3];

  if(!d->use_new_vst)
  {
    dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_precondition,
                              0, CLARG(dev_in), CLARG(dev_tmp),
      CLARG(width), CLARG(height), CLARG(aa), CLARG(sigma2));
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_precondition, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_precondition_v2, 0,
                              CLARG(dev_in), CLARG(dev_tmp),
                              CLARG(width), CLARG(height),
                              CLARG(aa), CLARG(p), CLARG(bb), CLARG(wb));
    err = dt_opencl_enqueue_kernel_2d(devid,
                                      gd->kernel_denoiseprofile_precondition_v2, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_init, 0,
                            CLARG(dev_U2), CLARG(width),
                            CLARG(height));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_init, sizes);
  if(err != CL_SUCCESS) goto error;

  const size_t bwidth = ROUNDUP(width, hblocksize);
  const size_t bheight = ROUNDUP(height, vblocksize);

  for(int kj_index = -K; kj_index <= 0; kj_index++)
  {
    for(int ki_index = -K; ki_index <= K; ki_index++)
    {
      // This formula is made for:
      // - ensuring that j = kj_index and i = ki_index when d->scattering is 0
      // - ensuring that no patch can appear twice (provided that d->scattering is
      //   in 0,1 range)
      // - avoiding grid artifacts by trying to take patches on various lines and columns
      const int abs_kj = abs(kj_index);
      const int abs_ki = abs(ki_index);
      const int j = scale * ((abs_kj * abs_kj * abs_kj
                              + 7.0 * abs_kj * sqrt(abs_ki)) * sign(kj_index)
                             * scattering / 6.0 + kj_index);
      const int i = scale * ((abs_ki * abs_ki * abs_ki
                              + 7.0 * abs_ki * sqrt(abs_kj)) * sign(ki_index)
                             * scattering / 6.0 + ki_index);
      int q[2] = { i, j };

      cl_mem dev_U4 = buckets[bucket_next(&state, NUM_BUCKETS)];
      dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_dist, 0,
                                CLARG(dev_tmp), CLARG(dev_U4),
                                CLARG(width), CLARG(height), CLARG(q));
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_dist, sizes);
      if(err != CL_SUCCESS) goto error;

      sizesl[0] = bwidth;
      sizesl[1] = ROUNDUPDHT(height, devid);
      sizesl[2] = 1;
      local[0] = hblocksize;
      local[1] = 1;
      local[2] = 1;
      cl_mem dev_U4_t = buckets[bucket_next(&state, NUM_BUCKETS)];
      dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_horiz,
                                0, CLARG(dev_U4), CLARG(dev_U4_t),
                                CLARG(width), CLARG(height), CLARG(q), CLARG(P),
                                CLLOCAL(sizeof(float) * (hblocksize + 2 * P)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid,
                                                   gd->kernel_denoiseprofile_horiz,
                                                   sizesl, local);
      if(err != CL_SUCCESS) goto error;

      sizesl[0] = ROUNDUPDWD(width, devid);
      sizesl[1] = bheight;
      sizesl[2] = 1;
      local[0] = 1;
      local[1] = vblocksize;
      local[2] = 1;
      cl_mem dev_U4_tt = buckets[bucket_next(&state, NUM_BUCKETS)];
      dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_vert,
                                0, CLARG(dev_U4_t), CLARG(dev_U4_tt),
                                CLARG(width), CLARG(height),
                                CLARG(q), CLARG(P), CLARG(norm),
                                CLLOCAL(sizeof(float) * (vblocksize + 2 * P)),
                                CLARG(central_pixel_weight), CLARG(dev_U4));
      err = dt_opencl_enqueue_kernel_2d_with_local
        (devid,
         gd->kernel_denoiseprofile_vert, sizesl, local);
      if(err != CL_SUCCESS) goto error;


      dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_accu,
                                0, CLARG(dev_tmp), CLARG(dev_U2),
                                CLARG(dev_U4_tt), CLARG(width),
                                CLARG(height), CLARG(q));
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_accu, sizes);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_finish_sync_pipe(devid, piece->pipe->type);

      // indirectly give gpu some air to breathe (and to do display related stuff)
      dt_iop_nap(dt_opencl_micro_nap(devid));
    }
  }

  if(!d->use_new_vst)
  {
    dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_finish, 0,
                              CLARG(dev_in), CLARG(dev_U2),
                              CLARG(dev_out), CLARG(width), CLARG(height),
                              CLARG(aa), CLARG(sigma2));
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_finish, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    const float bias = d->bias - 0.5 * logf(scale);
    dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_finish_v2, 0,
                              CLARG(dev_in), CLARG(dev_U2),
                              CLARG(dev_out), CLARG(width), CLARG(height),
                              CLARG(aa), CLARG(p), CLARG(bb), CLARG(bias), CLARG(wb));
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_finish_v2, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  for(int k = 0; k < NUM_BUCKETS; k++)
  {
    dt_opencl_release_mem_object(buckets[k]);
  }
  dt_opencl_release_mem_object(dev_U2);
  dt_opencl_release_mem_object(dev_tmp);
  return TRUE;

error:
  for(int k = 0; k < NUM_BUCKETS; k++)
  {
    dt_opencl_release_mem_object(buckets[k]);
  }
  dt_opencl_release_mem_object(dev_U2);
  dt_opencl_release_mem_object(dev_tmp);
  dt_print(DT_DEBUG_OPENCL,
           "[opencl_denoiseprofile] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
#endif /* USE_NEW_IMPL_CL */
}


static int process_wavelets_cl(struct dt_iop_module_t *self,
                               dt_dev_pixelpipe_iop_t *piece,
                               cl_mem dev_in,
                               cl_mem dev_out,
                               const dt_iop_roi_t *const roi_in,
                               const dt_iop_roi_t *const roi_out)
{
  dt_iop_denoiseprofile_data_t *d = (dt_iop_denoiseprofile_data_t *)piece->data;
  dt_iop_denoiseprofile_global_data_t *gd =
    (dt_iop_denoiseprofile_global_data_t *)self->global_data;

  const int max_max_scale = DT_IOP_DENOISE_PROFILE_BANDS; // hard limit
  int max_scale = 0;
  const float scale = fminf(roi_in->scale / piece->iscale, 1.0f);
  // largest desired filter on input buffer (20% of input dim)
  const float supp0
      = MIN(2 * (2u << (max_max_scale - 1)) + 1,
            MAX(piece->buf_in.height * piece->iscale,
                piece->buf_in.width * piece->iscale) * 0.2f);
  const float i0 = dt_log2f((supp0 - 1.0f) * .5f);
  for(; max_scale < max_max_scale; max_scale++)
  {
    // actual filter support on scaled buffer
    const float supp = 2 * (2u << max_scale) + 1;
    // approximates this filter size on unscaled input image:
    const float supp_in = supp * (1.0f / scale);
    const float i_in = dt_log2f((supp_in - 1) * .5f) - 1.0f;
    // i_in = max_scale .. .. .. 0
    const float t = 1.0f - (i_in + .5f) / i0;
    if(t < 0.0f) break;
  }

  const int devid = piece->pipe->devid;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const size_t npixels = (size_t)width * height;

  cl_mem dev_tmp = NULL;
  cl_mem dev_buf1 = NULL;
  cl_mem dev_buf2 = NULL;
  cl_mem dev_m = NULL;
  cl_mem dev_r = NULL;
  cl_mem dev_filter = NULL;
  cl_mem *dev_detail = calloc(max_max_scale, sizeof(cl_mem));
  float *sumsum = NULL;

  // corner case of extremely small image. this is not really likely
  // to happen but would cause issues later when we divide by
  // (n-1). so let's be prepared
  if(npixels < 2)
  {
    // copy original input from dev_in -> dev_out
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
    free(dev_detail);
    return TRUE;
  }

  dt_opencl_local_buffer_t flocopt
    = (dt_opencl_local_buffer_t){ .xoffset = 0,
                                  .xfactor = 1,
                                  .yoffset = 0,
                                  .yfactor = 1,
                                  .cellsize = 4 * sizeof(float),
                                  .overhead = 0,
                                  .sizex = 1u << 4,
                                  .sizey = 1u << 4 };

  if(!dt_opencl_local_buffer_opt(devid, gd->kernel_denoiseprofile_reduce_first, &flocopt))
    goto error;

  const size_t bwidth = ROUNDUP(width, flocopt.sizex);
  const size_t bheight = ROUNDUP(height, flocopt.sizey);

  const int bufsize = (bwidth / flocopt.sizex) * (bheight / flocopt.sizey);

  dt_opencl_local_buffer_t slocopt
    = (dt_opencl_local_buffer_t){ .xoffset = 0,
                                  .xfactor = 1,
                                  .yoffset = 0,
                                  .yfactor = 1,
                                  .cellsize = 4 * sizeof(float),
                                  .overhead = 0,
                                  .sizex = 1u << 16,
                                  .sizey = 1 };

  if(!dt_opencl_local_buffer_opt(devid, gd->kernel_denoiseprofile_reduce_first, &slocopt))
    goto error;

  const int reducesize = MIN(REDUCESIZE, ROUNDUP(bufsize, slocopt.sizex) / slocopt.sizex);

  dev_m = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * bufsize);
  if(dev_m == NULL) goto error;

  dev_r = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * reducesize);
  if(dev_r == NULL) goto error;

  sumsum = dt_alloc_align_float((size_t)4 * reducesize);
  if(sumsum == NULL) goto error;

  dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(dev_tmp == NULL) goto error;

  float m[] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f }; // 1/16, 4/16, 6/16, 4/16, 1/16
  float mm[5][5];
  for(int j = 0; j < 5; j++)
    for(int i = 0; i < 5; i++) mm[j][i] = m[i] * m[j];

  dev_filter = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 25, mm);
  if(dev_filter == NULL) goto error;

  for(int k = 0; k < max_scale; k++)
  {
    dev_detail[k] = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
    if(dev_detail[k] == NULL) goto error;
  }

  dt_aligned_pixel_t wb;  // the "unused" fourth element enables vectorization
  const dt_aligned_pixel_t wb_weights = { 2.0f, 1.0f, 2.0f, 0.0f };
  compute_wb_factors(wb,d,piece,wb_weights);
  wb[3] = 0.0f;

  // adaptive p depending on white balance
  const dt_aligned_pixel_t p = { MAX(d->shadows + 0.1 * logf(scale / wb[0]), 0.0f),
                                 MAX(d->shadows + 0.1 * logf(scale / wb[1]), 0.0f),
                                 MAX(d->shadows + 0.1 * logf(scale / wb[2]), 0.0f),
                                 1.0f};

  // conversion to Y0U0V0 space as defined in Secrets of image denoising cuisine
  dt_colormatrix_t toY0U0V0_tmp = { { 1.0f/3.0f, 1.0f/3.0f, 1.0f/3.0f },
                                    { 0.5f,      0.0f,      -0.5f },
                                    { 0.25f,     -0.5f,     0.25f } };

  // "unused" fourth element enables vectorization:
  dt_colormatrix_t toRGB_tmp = { { 0.0f, 0.0f, 0.0f },
                                 { 0.0f, 0.0f, 0.0f },
                                 { 0.0f, 0.0f, 0.0f } };
  set_up_conversion_matrices(toY0U0V0_tmp, toRGB_tmp, wb);

  // more strength in Y0U0V0 in order to get a similar smoothing as in other modes
  // otherwise, result was much less denoised in Y0U0V0 mode.
  const float compensate_strength = (d->wavelet_color_mode == MODE_RGB) ? 1.0f : 2.5f;

  // update the coeffs with strength and scale
  float toY0U0V0[9]; //TODO: change OpenCL kernels to use 3x4 matrices
  float toRGB[9] ;
  for(size_t k = 0; k < 3; k++)
    for(size_t c = 0; c < 3; c++)
    //(we can't use for_each_channel here because it can iterate over four elements)
    {
      toRGB[3*k+c] = toRGB_tmp[k][c] * d->strength * compensate_strength * scale;
      toY0U0V0[3*k+c] = toY0U0V0_tmp[k][c] / (d->strength * compensate_strength * scale);
    }

  // update the coeffs with strength and scale
  for_each_channel(i) wb[i] *= d->strength * compensate_strength * scale;

  dt_aligned_pixel_t aa = { d->a[1] * wb[0], d->a[1] * wb[1], d->a[1] * wb[2], 1.0f };
  dt_aligned_pixel_t bb = { d->b[1] * wb[0], d->b[1] * wb[1], d->b[1] * wb[2], 1.0f };

  const dt_aligned_pixel_t sigma2 = { (bb[0] / aa[0]) * (bb[0] / aa[0]),
                                      (bb[1] / aa[1]) * (bb[1] / aa[1]),
                                      (bb[2] / aa[2]) * (bb[2] / aa[2]),
                                      0.0f };

  const float compensate_p =
    DT_IOP_DENOISE_PROFILE_P_FULCRUM / powf(DT_IOP_DENOISE_PROFILE_P_FULCRUM, d->shadows);
  if(d->use_new_vst)
  {
    for_each_channel(c)
    {
      aa[c] = d->a[1] * compensate_p;
      bb[c] = d->b[1];
    }
  }

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  if(!d->use_new_vst)
  {
    dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_precondition,
                              0, CLARG(dev_in), CLARG(dev_out),
                              CLARG(width), CLARG(height), CLARG(aa), CLARG(sigma2));
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_precondition, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  else if(d->wavelet_color_mode == MODE_RGB)
  {
    dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_precondition_v2,
                              0, CLARG(dev_in), CLARG(dev_out),
                              CLARG(width), CLARG(height),
                              CLARG(aa), CLARG(p), CLARG(bb), CLARG(wb));
    err = dt_opencl_enqueue_kernel_2d(devid,
                                      gd->kernel_denoiseprofile_precondition_v2, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    cl_mem dev_Y0U0V0 = NULL;
    dev_Y0U0V0 = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 9, toY0U0V0);
    if(dev_Y0U0V0 != NULL)
    {
      dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_precondition_Y0U0V0,
                                0, CLARG(dev_in),
                                CLARG(dev_out), CLARG(width), CLARG(height),
                                CLARG(aa), CLARG(p), CLARG(bb), CLARG(dev_Y0U0V0));
      err = dt_opencl_enqueue_kernel_2d(devid,
                                        gd->kernel_denoiseprofile_precondition_Y0U0V0,
                                        sizes);
      dt_opencl_release_mem_object(dev_Y0U0V0);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      dt_opencl_release_mem_object(dev_Y0U0V0);
      goto error;
    }
  }

  dev_buf1 = dev_out;
  dev_buf2 = dev_tmp;

  /* decompose image into detail scales and coarse */
  for(int s = 0; s < max_scale; s++)
  {
    const float sigma = 1.0f;
    const float varf = sqrtf(2.0f + 2.0f * 4.0f * 4.0f + 6.0f * 6.0f) / 16.0f; // about 0.5
    const float sigma_band = powf(varf, s) * sigma;
    const float inv_sigma2 = 1.0f / (sigma_band * sigma_band);

    dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_decompose,
                              0, CLARG(dev_buf1), CLARG(dev_buf2),
                              CLARG(dev_detail[s]), CLARG(width), CLARG(height),
                              CLARG(s), CLARG(inv_sigma2), CLARG(dev_filter));
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_decompose, sizes);
    if(err != CL_SUCCESS) goto error;

    // indirectly give gpu some air to breathe (and to do display related stuff)
    dt_iop_nap(dt_opencl_micro_nap(devid));

    // swap buffers
    cl_mem dev_buf3 = dev_buf2;
    dev_buf2 = dev_buf1;
    dev_buf1 = dev_buf3;
  }

  /* now synthesize again */
  for(int s = max_scale - 1; s >= 0; s--)
  {
    // variance stabilizing transform maps sigma to unity.
    const float sigma = 1.0f;
    // it is then transformed by wavelet scales via the 5 tap a-trous filter:
    const float varf = sqrtf(2.0f + 2.0f * 4.0f * 4.0f + 6.0f * 6.0f) / 16.0f; // about 0.5
    const float sigma_band = powf(varf, s) * sigma;

    // determine thrs as bayesshrink
    dt_aligned_pixel_t sum_y2 = { 0.0f };

    size_t lsizes[3];
    size_t llocal[3];

    lsizes[0] = bwidth;
    lsizes[1] = bheight;
    lsizes[2] = 1;
    llocal[0] = flocopt.sizex;
    llocal[1] = flocopt.sizey;
    llocal[2] = 1;
    dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_reduce_first,
                              0, CLARG((dev_detail[s])),
                              CLARG(width), CLARG(height),
                              CLARG(dev_m),
                              CLLOCAL(sizeof(float) * 4 * flocopt.sizex * flocopt.sizey));
    err = dt_opencl_enqueue_kernel_2d_with_local
      (devid,
       gd->kernel_denoiseprofile_reduce_first, lsizes,
       llocal);
    if(err != CL_SUCCESS) goto error;


    lsizes[0] = (size_t)reducesize * slocopt.sizex;
    lsizes[1] = 1;
    lsizes[2] = 1;
    llocal[0] = slocopt.sizex;
    llocal[1] = 1;
    llocal[2] = 1;
    dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_reduce_second,
                              0, CLARG(dev_m), CLARG(dev_r),
                              CLARG(bufsize), CLLOCAL(sizeof(float) * 4 * slocopt.sizex));
    err = dt_opencl_enqueue_kernel_2d_with_local
      (devid,
       gd->kernel_denoiseprofile_reduce_second, lsizes,
       llocal);
    if(err != CL_SUCCESS) goto error;

    err = dt_opencl_read_buffer_from_device(devid, (void *)sumsum, dev_r, 0,
                                            sizeof(float) * 4 * reducesize, CL_TRUE);
    if(err != CL_SUCCESS)
      goto error;

    for(int k = 0; k < reducesize; k++)
    {
      for_each_channel(c)
      {
        sum_y2[c] += sumsum[4 * k + c];
      }
    }

    const float sb2 = sigma_band * sigma_band;
    const dt_aligned_pixel_t var_y = { sum_y2[0] / (npixels - 1.0f),
                                       sum_y2[1] / (npixels - 1.0f),
                                       sum_y2[2] / (npixels - 1.0f),
                                       0.0f };

    const dt_aligned_pixel_t std_x = { sqrtf(MAX(1e-6f, var_y[0] - sb2)),
                                       sqrtf(MAX(1e-6f, var_y[1] - sb2)),
                                       sqrtf(MAX(1e-6f, var_y[2] - sb2)),
                                       1.0f };
    // add 8.0 here because it seemed a little weak
    dt_aligned_pixel_t adjt = { 8.0f, 8.0f, 8.0f, 0.0f };

    const int offset_scale = DT_IOP_DENOISE_PROFILE_BANDS - max_scale;
    const int band_index = DT_IOP_DENOISE_PROFILE_BANDS - (s + offset_scale + 1);

    if(d->wavelet_color_mode == MODE_RGB)
    {
      // current scale number is s+offset_scale
      // for instance, largest s is DT_IOP_DENOISE_PROFILE_BANDS
      // max_scale only indicates the number of scales to process at THIS
      // zoom level, it does NOT corresponds to the the maximum number of scales.
      // in other words, max_s is the maximum number of VISIBLE scales.
      // That is why we have this "s+offset_scale"
      float band_force_exp_2 = d->force[DT_DENOISE_PROFILE_ALL][band_index];
      band_force_exp_2 *= band_force_exp_2;
      band_force_exp_2 *= 4;
      for_each_channel(ch)
      {
        adjt[ch] *= band_force_exp_2;
      }
      band_force_exp_2 = d->force[DT_DENOISE_PROFILE_R][band_index];
      band_force_exp_2 *= band_force_exp_2;
      band_force_exp_2 *= 4;
      adjt[0] *= band_force_exp_2;
      band_force_exp_2 = d->force[DT_DENOISE_PROFILE_G][band_index];
      band_force_exp_2 *= band_force_exp_2;
      band_force_exp_2 *= 4;
      adjt[1] *= band_force_exp_2;
      band_force_exp_2 = d->force[DT_DENOISE_PROFILE_B][band_index];
      band_force_exp_2 *= band_force_exp_2;
      band_force_exp_2 *= 4;
      adjt[2] *= band_force_exp_2;
    }
    else
    {
      float band_force_exp_2 = d->force[DT_DENOISE_PROFILE_Y0][band_index];
      band_force_exp_2 *= band_force_exp_2;
      band_force_exp_2 *= 4;
      adjt[0] *= band_force_exp_2;
      band_force_exp_2 = d->force[DT_DENOISE_PROFILE_U0V0][band_index];
      band_force_exp_2 *= band_force_exp_2;
      band_force_exp_2 *= 4;
      adjt[1] *= band_force_exp_2;
      adjt[2] *= band_force_exp_2;
    }

    const dt_aligned_pixel_t thrs = { adjt[0] * sb2 / std_x[0],
                                      adjt[1] * sb2 / std_x[1],
                                      adjt[2] * sb2 / std_x[2],
                                      0.0f };
    // fprintf(stderr, "scale %d thrs %f %f %f\n", s, thrs[0], thrs[1], thrs[2]);

    const dt_aligned_pixel_t boost = { 1.0f, 1.0f, 1.0f, 1.0f };

    dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_synthesize,
                              0, CLARG(dev_buf1), CLARG(dev_detail[s]),
                              CLARG(dev_buf2), CLARG(width), CLARG(height),
                              CLARG(thrs[0]), CLARG(thrs[1]), CLARG(thrs[2]),
                              CLARG(thrs[3]), CLARG(boost[0]), CLARG(boost[1]),
                              CLARG(boost[2]), CLARG(boost[3]));
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_synthesize, sizes);
    if(err != CL_SUCCESS) goto error;

    // indirectly give gpu some air to breathe (and to do display related stuff)
    dt_iop_nap(dt_opencl_micro_nap(devid));

    // swap buffers
    cl_mem dev_buf3 = dev_buf2;
    dev_buf2 = dev_buf1;
    dev_buf1 = dev_buf3;
  }

  // copy output of last run of synthesize kernel to dev_tmp (if not
  // already there) note: we need to take swap of buffers into
  // account, so current output lies in dev_buf1
  if(dev_buf1 != dev_tmp)
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_buf1, dev_tmp, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
  }

  if(!d->use_new_vst)
  {
    dt_opencl_set_kernel_args(devid, gd->kernel_denoiseprofile_backtransform,
                              0, CLARG(dev_tmp), CLARG(dev_out),
                              CLARG(width), CLARG(height), CLARG(aa), CLARG(sigma2));
    err = dt_opencl_enqueue_kernel_2d(devid,
                                      gd->kernel_denoiseprofile_backtransform, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  else if(d->wavelet_color_mode == MODE_RGB)
  {
    const float bias = d->bias - 0.5 * logf(scale);
    dt_opencl_set_kernel_args(devid,
                              gd->kernel_denoiseprofile_backtransform_v2, 0,
                              CLARG(dev_tmp),
                              CLARG(dev_out), CLARG(width), CLARG(height),
                              CLARG(aa), CLARG(p), CLARG(bb), CLARG(bias), CLARG(wb));
    err = dt_opencl_enqueue_kernel_2d(devid,
                                      gd->kernel_denoiseprofile_backtransform_v2, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    cl_mem dev_RGB = NULL;
    dev_RGB = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 9, toRGB);
    if(dev_RGB != NULL)
    {
      const float bias = d->bias - 0.5 * logf(scale);
      dt_opencl_set_kernel_args(devid,
                                gd->kernel_denoiseprofile_backtransform_Y0U0V0, 0,
                                CLARG(dev_tmp),
                                CLARG(dev_out), CLARG(width), CLARG(height),
                                CLARG(aa), CLARG(p), CLARG(bb), CLARG(bias), CLARG(wb),
        CLARG(dev_RGB));
      err = dt_opencl_enqueue_kernel_2d(devid,
                                        gd->kernel_denoiseprofile_backtransform_Y0U0V0,
                                        sizes);
      dt_opencl_release_mem_object(dev_RGB);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      dt_opencl_release_mem_object(dev_RGB);
      goto error;
    }
  }

  dt_opencl_finish_sync_pipe(devid, piece->pipe->type);

  dt_opencl_release_mem_object(dev_r);
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_filter);
  for(int k = 0; k < max_scale; k++)
    dt_opencl_release_mem_object(dev_detail[k]);
  free(dev_detail);
  dt_free_align(sumsum);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_r);
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_filter);
  for(int k = 0; k < max_scale; k++)
    dt_opencl_release_mem_object(dev_detail[k]);
  free(dev_detail);
  dt_free_align(sumsum);
  dt_print(DT_DEBUG_OPENCL,
           "[opencl_denoiseprofile] couldn't enqueue kernel! %s, devid %d\n",
           cl_errstr(err), devid);
  return FALSE;
}

int process_cl(struct dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_denoiseprofile_params_t *d = (dt_iop_denoiseprofile_params_t *)piece->data;

  if(d->mode == MODE_NLMEANS || d->mode == MODE_NLMEANS_AUTO)
  {
    return process_nlmeans_cl(self, piece, dev_in, dev_out, roi_in, roi_out);
  }
  else if(d->mode == MODE_WAVELETS || d->mode == MODE_WAVELETS_AUTO)
  {
    return process_wavelets_cl(self, piece, dev_in, dev_out, roi_in, roi_out);
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_denoiseprofile] compute variance not yet supported by opencl code\n");
    return FALSE;
  }
}
#endif // HAVE_OPENCL

void process(struct dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  dt_iop_denoiseprofile_params_t *d = (dt_iop_denoiseprofile_params_t *)piece->data;

  if(d->mode == MODE_NLMEANS
     || d->mode == MODE_NLMEANS_AUTO)
    process_nlmeans(self, piece, ivoid, ovoid, roi_in, roi_out);
  else if(d->mode == MODE_WAVELETS
          || d->mode == MODE_WAVELETS_AUTO)
    process_wavelets(self, piece, ivoid, ovoid, roi_in, roi_out,
                     eaw_dn_decompose, eaw_synthesize);
  else
    process_variance(self, piece, ivoid, ovoid, roi_in, roi_out);
}

static inline unsigned infer_radius_from_profile(const float a)
{
  return MIN((unsigned)(1.0f + a * 15000.0f + a * a * 300000.0f), 8);
}

static inline float infer_scattering_from_profile(const float a)
{
  return MIN(3000.0f * a, 1.0f);
}

static inline float infer_shadows_from_profile(const float a)
{
  return MIN(MAX(0.1f - 0.1 * logf(a), 0.7f), 1.8f);
}

static inline float infer_bias_from_profile(const float a)
{
  return -MAX(5 + 0.5 * logf(a), 0.0);
}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);

  dt_iop_denoiseprofile_params_t *d = module->default_params;

  for(int k = 0; k < DT_IOP_DENOISE_PROFILE_BANDS; k++)
  {
    for(int ch = 0; ch < DT_DENOISE_PROFILE_NONE; ch++)
    {
      d->x[ch][k] = k / (DT_IOP_DENOISE_PROFILE_BANDS - 1.f);
    }
  }
}

/** this will be called to init new defaults if a new image is loaded
 * from film strip mode. */
void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_denoiseprofile_gui_data_t *g =
    (dt_iop_denoiseprofile_gui_data_t *)module->gui_data;
  dt_iop_denoiseprofile_params_t *d = module->default_params;

  d->radius = 1.0f;
  d->nbhood = 7.0f;
  d->strength = 1.0f;
  d->shadows = 1.0f;
  d->bias = 0.0f;
  d->scattering = 0.0f;
  d->central_pixel_weight = 0.1f;
  d->overshooting = 1.0f;
  d->mode = MODE_WAVELETS;
  d->wb_adaptive_anscombe = TRUE;
  d->fix_anscombe_and_nlmeans_norm = TRUE;
  d->use_new_vst = TRUE;
  d->wavelet_color_mode = MODE_Y0U0V0;

  GList *profiles = dt_noiseprofile_get_matching(&module->dev->image_storage);
  const int iso = module->dev->image_storage.exif_iso;

  // default to generic poissonian
  dt_noiseprofile_t interpolated = dt_noiseprofile_generic;

  char name[512];

  g_strlcpy(name, _(interpolated.name), sizeof(name));

  dt_noiseprofile_t *last = NULL;
  for(GList *iter = profiles; iter; iter = g_list_next(iter))
  {
    dt_noiseprofile_t *current = (dt_noiseprofile_t *)iter->data;

    if(current->iso == iso)
    {
      interpolated = *current;
      // signal later autodetection in commit_params:
      interpolated.a[0] = -1.0f;
      snprintf(name, sizeof(name), _("found match for ISO %d"), iso);
      break;
    }
    if(last && last->iso < iso && current->iso > iso)
    {
      interpolated.iso = iso;
      dt_noiseprofile_interpolate(last, current, &interpolated);
      // signal later autodetection in commit_params:
      interpolated.a[0] = -1.0f;
      snprintf(name, sizeof(name), _("interpolated from ISO %d and %d"),
               last->iso, current->iso);
      break;
    }
    last = current;
  }

  const float a = interpolated.a[1];

  d->radius = infer_radius_from_profile(a);
  d->scattering = infer_scattering_from_profile(a);
  d->shadows = infer_shadows_from_profile(a);
  d->bias = infer_bias_from_profile(a);

  for(int k = 0; k < 3; k++)
  {
    d->a[k] = interpolated.a[k];
    d->b[k] = interpolated.b[k];
  }

  if(g)
  {
    dt_bauhaus_combobox_clear(g->profile);

    // get matching profiles:
    if(g->profiles)
      g_list_free_full(g->profiles, dt_noiseprofile_free);
    g->profiles = profiles;
    g->interpolated = interpolated;

    dt_bauhaus_combobox_add(g->profile, name);
    for(GList *iter = g->profiles; iter; iter = g_list_next(iter))
    {
      dt_noiseprofile_t *profile = (dt_noiseprofile_t *)iter->data;
      dt_bauhaus_combobox_add(g->profile, profile->name);
    }
    dt_bauhaus_combobox_set(g->profile, 0);

    gui_update(module);
  }
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 11; // denoiseprofile.cl, from programs.conf
  dt_iop_denoiseprofile_global_data_t *gd =
    (dt_iop_denoiseprofile_global_data_t *)
      malloc(sizeof(dt_iop_denoiseprofile_global_data_t));

  module->data = gd;
  gd->kernel_denoiseprofile_precondition =
    dt_opencl_create_kernel(program, "denoiseprofile_precondition");
  gd->kernel_denoiseprofile_precondition_v2 =
    dt_opencl_create_kernel(program, "denoiseprofile_precondition_v2");
  gd->kernel_denoiseprofile_precondition_Y0U0V0 =
    dt_opencl_create_kernel(program, "denoiseprofile_precondition_Y0U0V0");
  gd->kernel_denoiseprofile_init =
    dt_opencl_create_kernel(program, "denoiseprofile_init");
  gd->kernel_denoiseprofile_dist =
    dt_opencl_create_kernel(program, "denoiseprofile_dist");
  gd->kernel_denoiseprofile_horiz =
    dt_opencl_create_kernel(program, "denoiseprofile_horiz");
  gd->kernel_denoiseprofile_vert =
    dt_opencl_create_kernel(program, "denoiseprofile_vert");
  gd->kernel_denoiseprofile_accu =
    dt_opencl_create_kernel(program, "denoiseprofile_accu");
  gd->kernel_denoiseprofile_finish =
    dt_opencl_create_kernel(program, "denoiseprofile_finish");
  gd->kernel_denoiseprofile_finish_v2 =
    dt_opencl_create_kernel(program, "denoiseprofile_finish_v2");
  gd->kernel_denoiseprofile_backtransform =
    dt_opencl_create_kernel(program, "denoiseprofile_backtransform");
  gd->kernel_denoiseprofile_backtransform_v2 =
    dt_opencl_create_kernel(program, "denoiseprofile_backtransform_v2");
  gd->kernel_denoiseprofile_backtransform_Y0U0V0 =
    dt_opencl_create_kernel(program, "denoiseprofile_backtransform_Y0U0V0");
  gd->kernel_denoiseprofile_decompose =
    dt_opencl_create_kernel(program, "denoiseprofile_decompose");
  gd->kernel_denoiseprofile_synthesize =
    dt_opencl_create_kernel(program, "denoiseprofile_synthesize");
  gd->kernel_denoiseprofile_reduce_first =
    dt_opencl_create_kernel(program, "denoiseprofile_reduce_first");
  gd->kernel_denoiseprofile_reduce_second =
    dt_opencl_create_kernel(program, "denoiseprofile_reduce_second");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_denoiseprofile_global_data_t *gd =
    (dt_iop_denoiseprofile_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_precondition);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_precondition_v2);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_init);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_dist);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_horiz);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_vert);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_accu);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_finish);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_finish_v2);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_backtransform);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_backtransform_v2);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_decompose);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_synthesize);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_reduce_first);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_reduce_second);
  free(module->data);
  module->data = NULL;
}

static dt_noiseprofile_t dt_iop_denoiseprofile_get_auto_profile(dt_iop_module_t *self)
{
  GList *profiles = dt_noiseprofile_get_matching(&self->dev->image_storage);
  dt_noiseprofile_t interpolated = dt_noiseprofile_generic; // default to generic poissonian

  const int iso = self->dev->image_storage.exif_iso;
  dt_noiseprofile_t *last = NULL;
  for(GList *iter = profiles; iter; iter = g_list_next(iter))
  {
    dt_noiseprofile_t *current = (dt_noiseprofile_t *)iter->data;
    if(current->iso == iso)
    {
      interpolated = *current;
      break;
    }
    if(last && last->iso < iso && current->iso > iso)
    {
      interpolated.iso = iso;
      dt_noiseprofile_interpolate(last, current, &interpolated);
      break;
    }
    last = current;
  }
  g_list_free_full(profiles, dt_noiseprofile_free);
  return interpolated;
}

/** commit is the synch point between core and gui, so it copies
 * params to pipe data. */
void commit_params(struct dt_iop_module_t *self,
                   dt_iop_params_t *params,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_denoiseprofile_params_t *p = (dt_iop_denoiseprofile_params_t *)params;
  dt_iop_denoiseprofile_data_t *d = (dt_iop_denoiseprofile_data_t *)piece->data;

  d->nbhood = p->nbhood;
  d->central_pixel_weight = p->central_pixel_weight;
  d->strength = p->strength;
  d->overshooting = p->overshooting;
  for(int i = 0; i < 3; i++)
  {
    d->a[i] = p->a[i];
    d->b[i] = p->b[i];
  }
  d->mode = p->mode;
  d->wavelet_color_mode = p->wavelet_color_mode;

  // compare if a[0] in params is set to "magic value" -1.0 for autodetection
  if(p->a[0] == -1.0)
  {
    // autodetect matching profile again, the same way as detecting their names,
    // this is partially duplicated code and data because we are not allowed to access
    // gui_data here ..
    dt_noiseprofile_t interpolated = dt_iop_denoiseprofile_get_auto_profile(self);
    for(int k = 0; k < 3; k++)
    {
      d->a[k] = interpolated.a[k];
      d->b[k] = interpolated.b[k];
    }
  }

  if((p->mode == MODE_NLMEANS_AUTO) || (p->mode == MODE_WAVELETS_AUTO))
  {
    const float gain = p->overshooting;
    d->radius = infer_radius_from_profile(d->a[1] * gain);
    d->scattering = infer_scattering_from_profile(d->a[1] * gain);
    d->shadows = infer_shadows_from_profile(d->a[1] * gain);
    d->bias = infer_bias_from_profile(d->a[1] * gain);
  }
  else
  {
    d->radius = p->radius;
    d->scattering = p->scattering;
    d->shadows = p->shadows;
    d->bias = p->bias;
  }

  for(int ch = 0; ch < DT_DENOISE_PROFILE_NONE; ch++)
  {
    dt_draw_curve_set_point(d->curve[ch], 0,
                            p->x[ch][DT_IOP_DENOISE_PROFILE_BANDS - 2] - 1.f,
                            p->y[ch][0]);
    for(int k = 0; k < DT_IOP_DENOISE_PROFILE_BANDS; k++)
      dt_draw_curve_set_point(d->curve[ch], k, p->x[ch][k], p->y[ch][k]);
    dt_draw_curve_set_point(d->curve[ch], DT_IOP_DENOISE_PROFILE_BANDS + 1,
                            p->x[ch][1] + 1.f,
                            p->y[ch][DT_IOP_DENOISE_PROFILE_BANDS - 1]);
    dt_draw_curve_calc_values(d->curve[ch], 0.0, 1.0,
                              DT_IOP_DENOISE_PROFILE_BANDS, NULL, d->force[ch]);
  }

  d->wb_adaptive_anscombe = p->wb_adaptive_anscombe;
  d->fix_anscombe_and_nlmeans_norm = p->fix_anscombe_and_nlmeans_norm;
  d->use_new_vst = p->use_new_vst;
}

void init_pipe(struct dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_denoiseprofile_data_t *d =
    (dt_iop_denoiseprofile_data_t *)malloc(sizeof(dt_iop_denoiseprofile_data_t));
  dt_iop_denoiseprofile_params_t *default_params =
    (dt_iop_denoiseprofile_params_t *)self->default_params;

  piece->data = (void *)d;
  for(int ch = 0; ch < DT_DENOISE_PROFILE_NONE; ch++)
  {
    d->curve[ch] = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
    for(int k = 0; k < DT_IOP_DENOISE_PROFILE_BANDS; k++)
      (void)dt_draw_curve_add_point(d->curve[ch], default_params->x[ch][k],
                                    default_params->y[ch][k]);
  }
}

void cleanup_pipe(struct dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_denoiseprofile_data_t *d = (dt_iop_denoiseprofile_data_t *)(piece->data);
  for(int ch = 0; ch < DT_DENOISE_PROFILE_NONE; ch++) dt_draw_curve_destroy(d->curve[ch]);
  free(piece->data);
  piece->data = NULL;
}

static void profile_callback(GtkWidget *w, dt_iop_module_t *self)
{
  int i = dt_bauhaus_combobox_get(w);
  dt_iop_denoiseprofile_params_t *p = (dt_iop_denoiseprofile_params_t *)self->params;
  dt_iop_denoiseprofile_gui_data_t *g = (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;
  const dt_noiseprofile_t *profile = &(g->interpolated);
  if(i > 0) profile = (dt_noiseprofile_t *)g_list_nth_data(g->profiles, i - 1);
  for(int k = 0; k < 3; k++)
  {
    p->a[k] = profile->a[k];
    p->b[k] = profile->b[k];
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void mode_callback(GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_denoiseprofile_params_t *p = (dt_iop_denoiseprofile_params_t *)self->params;
  dt_iop_denoiseprofile_gui_data_t *g = (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;
  const unsigned mode = dt_bauhaus_combobox_get(w);
  switch(mode)
  {
    case 0:
      p->mode = MODE_NLMEANS;
      gtk_widget_hide(g->box_wavelets);
      gtk_widget_hide(g->box_variance);
      gtk_widget_show_all(g->box_nlm);
      break;
    case 1:
      p->mode = MODE_NLMEANS_AUTO;
      gtk_widget_hide(g->box_wavelets);
      gtk_widget_hide(g->box_variance);
      gtk_widget_show_all(g->box_nlm);
      gtk_widget_set_visible(g->radius, FALSE);
      gtk_widget_set_visible(g->nbhood, FALSE);
      gtk_widget_set_visible(g->scattering, FALSE);
      break;
    case 2:
      p->mode = MODE_WAVELETS;
      gtk_widget_hide(g->box_nlm);
      gtk_widget_hide(g->box_variance);
      gtk_widget_show_all(g->box_wavelets);
      gtk_widget_set_visible(GTK_WIDGET(g->wavelet_color_mode), p->use_new_vst);
      gtk_widget_set_visible(GTK_WIDGET(g->channel_tabs),
                             p->use_new_vst && (p->wavelet_color_mode == MODE_RGB));
      gtk_widget_set_visible(GTK_WIDGET(g->channel_tabs_Y0U0V0),
                             p->use_new_vst && (p->wavelet_color_mode == MODE_Y0U0V0));
      break;
    case 3:
      p->mode = MODE_WAVELETS_AUTO;
      gtk_widget_hide(g->box_nlm);
      gtk_widget_hide(g->box_variance);
      gtk_widget_show_all(g->box_wavelets);
      gtk_widget_set_visible(GTK_WIDGET(g->wavelet_color_mode), p->use_new_vst);
      gtk_widget_set_visible(GTK_WIDGET(g->channel_tabs),
                             p->use_new_vst && (p->wavelet_color_mode == MODE_RGB));
      gtk_widget_set_visible(GTK_WIDGET(g->channel_tabs_Y0U0V0),
                             p->use_new_vst && (p->wavelet_color_mode == MODE_Y0U0V0));
      break;
    case 4:
      p->mode = MODE_VARIANCE;
      gtk_widget_hide(g->box_wavelets);
      gtk_widget_hide(g->box_nlm);
      gtk_widget_show_all(g->box_variance);
      break;
  }
  const gboolean auto_mode =
    (p->mode == MODE_NLMEANS_AUTO) || (p->mode == MODE_WAVELETS_AUTO);
  gtk_widget_set_visible(g->shadows, p->use_new_vst && !auto_mode);
  gtk_widget_set_visible(g->bias, p->use_new_vst && !auto_mode);
  gtk_widget_set_visible(g->overshooting, auto_mode);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_denoiseprofile_params_t *p = (dt_iop_denoiseprofile_params_t *)self->params;
  dt_iop_denoiseprofile_gui_data_t *g =
    (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;

  if(w == g->wavelet_color_mode)
  {
    gtk_widget_set_visible(GTK_WIDGET(g->channel_tabs),
                           p->wavelet_color_mode == MODE_RGB);
    gtk_widget_set_visible(GTK_WIDGET(g->channel_tabs_Y0U0V0),
                           p->wavelet_color_mode == MODE_Y0U0V0);
    if(p->wavelet_color_mode == MODE_RGB)
      g->channel = DT_DENOISE_PROFILE_ALL;
    else
      g->channel = DT_DENOISE_PROFILE_Y0;
  }
  else if(w == g->overshooting)
  {
    const float gain = p->overshooting;
    float a = p->a[1];
    if(p->a[0] == -1.0)
    {
      dt_noiseprofile_t interpolated = dt_iop_denoiseprofile_get_auto_profile(self);
      a = interpolated.a[1];
    }
    // set the sliders as visible while we are setting their values
    // otherwise a log message appears
    if(p->mode == MODE_NLMEANS_AUTO)
    {
      gtk_widget_set_visible(g->radius, TRUE);
      gtk_widget_set_visible(g->scattering, TRUE);
      dt_bauhaus_slider_set(g->radius, infer_radius_from_profile(a * gain));
      dt_bauhaus_slider_set(g->scattering, infer_scattering_from_profile(a * gain));
      gtk_widget_set_visible(g->radius, FALSE);
      gtk_widget_set_visible(g->scattering, FALSE);
    }
    else
    {
      // we are in wavelets mode.
      // we need to show the box_nlm, setting the sliders to visible is not enough
      gtk_widget_show_all(g->box_nlm);
      dt_bauhaus_slider_set(g->radius, infer_radius_from_profile(a * gain));
      dt_bauhaus_slider_set(g->scattering, infer_scattering_from_profile(a * gain));
      gtk_widget_hide(g->box_nlm);
    }
    gtk_widget_set_visible(g->shadows, TRUE);
    gtk_widget_set_visible(g->bias, TRUE);
    dt_bauhaus_slider_set(g->shadows, infer_shadows_from_profile(a * gain));
    dt_bauhaus_slider_set(g->bias, infer_bias_from_profile(a * gain));
    gtk_widget_set_visible(g->shadows, FALSE);
    gtk_widget_set_visible(g->bias, FALSE);
  }
  else if(w == g->use_new_vst)
  {
    const gboolean auto_mode =
      (p->mode == MODE_NLMEANS_AUTO) || (p->mode == MODE_WAVELETS_AUTO);
    gtk_widget_set_visible(g->shadows, p->use_new_vst && !auto_mode);
    gtk_widget_set_visible(g->bias, p->use_new_vst && !auto_mode);
    gtk_widget_set_visible(g->wavelet_color_mode, p->use_new_vst);

    if(!p->use_new_vst
       && p->wavelet_color_mode == MODE_Y0U0V0)
      p->wavelet_color_mode = MODE_RGB;
  }
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_denoiseprofile_gui_data_t *g = (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;
  dt_iop_denoiseprofile_params_t *p = (dt_iop_denoiseprofile_params_t *)self->params;

  dt_bauhaus_combobox_set(g->profile, -1);
  unsigned combobox_index = 0;
  switch(p->mode)
  {
    case MODE_NLMEANS:
      combobox_index = 0;
      gtk_widget_hide(g->box_wavelets);
      gtk_widget_hide(g->box_variance);
      gtk_widget_show_all(g->box_nlm);
      break;
    case MODE_NLMEANS_AUTO:
      combobox_index = 1;
      gtk_widget_hide(g->box_wavelets);
      gtk_widget_hide(g->box_variance);
      gtk_widget_show_all(g->box_nlm);
      gtk_widget_set_visible(g->radius, FALSE);
      gtk_widget_set_visible(g->nbhood, FALSE);
      gtk_widget_set_visible(g->scattering, FALSE);
      break;
    case MODE_WAVELETS:
      combobox_index = 2;
      gtk_widget_hide(g->box_nlm);
      gtk_widget_hide(g->box_variance);
      gtk_widget_show_all(g->box_wavelets);
      break;
    case MODE_WAVELETS_AUTO:
      combobox_index = 3;
      gtk_widget_hide(g->box_nlm);
      gtk_widget_hide(g->box_variance);
      gtk_widget_show_all(g->box_wavelets);
      break;
    case MODE_VARIANCE:
      combobox_index = 4;
      gtk_widget_hide(g->box_wavelets);
      gtk_widget_hide(g->box_nlm);
      gtk_widget_show_all(g->box_variance);
      if(dt_bauhaus_combobox_length(g->mode) == 4)
      {
        dt_bauhaus_combobox_add(g->mode, _("compute variance"));
      }
      break;
  }
  float a = p->a[1];
  if(p->a[0] == -1.0)
  {
    dt_noiseprofile_t interpolated = dt_iop_denoiseprofile_get_auto_profile(self);
    a = interpolated.a[1];
  }
  if((p->mode == MODE_NLMEANS_AUTO) || (p->mode == MODE_WAVELETS_AUTO))
  {
    const float gain = p->overshooting;
    dt_bauhaus_slider_set(g->radius, infer_radius_from_profile(a * gain));
    dt_bauhaus_slider_set(g->scattering, infer_scattering_from_profile(a * gain));
    dt_bauhaus_slider_set(g->shadows, infer_shadows_from_profile(a * gain));
    dt_bauhaus_slider_set(g->bias, infer_bias_from_profile(a * gain));
  }
  dt_bauhaus_combobox_set(g->mode, combobox_index);
  if(p->a[0] == -1.0)
  {
    dt_bauhaus_combobox_set(g->profile, 0);
  }
  else
  {
    int i = 1;
    for(GList *iter = g->profiles; iter; iter = g_list_next(iter), i++)
    {
      dt_noiseprofile_t *profile = (dt_noiseprofile_t *)iter->data;
      if(!memcmp(profile->a, p->a, sizeof(float) * 3)
         && !memcmp(profile->b, p->b, sizeof(float) * 3))
      {
        dt_bauhaus_combobox_set(g->profile, i);
        break;
      }
    }
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->wb_adaptive_anscombe),
                               p->wb_adaptive_anscombe);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->fix_anscombe_and_nlmeans_norm),
                               p->fix_anscombe_and_nlmeans_norm);
  gtk_widget_set_visible(g->fix_anscombe_and_nlmeans_norm,
                         !p->fix_anscombe_and_nlmeans_norm);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->use_new_vst), p->use_new_vst);
  gtk_widget_set_visible(g->use_new_vst, !p->use_new_vst);
  const gboolean auto_mode =
    (p->mode == MODE_NLMEANS_AUTO) || (p->mode == MODE_WAVELETS_AUTO);
  const gboolean wavelet_mode =
    (p->mode == MODE_WAVELETS) || (p->mode == MODE_WAVELETS_AUTO);
  gtk_widget_set_visible(g->overshooting, auto_mode);
  gtk_widget_set_visible(g->wavelet_color_mode, p->use_new_vst && wavelet_mode);
  gtk_widget_set_visible(g->shadows, p->use_new_vst && !auto_mode);
  gtk_widget_set_visible(g->bias, p->use_new_vst && !auto_mode);
  gtk_widget_set_visible(GTK_WIDGET(g->channel_tabs),
                         p->wavelet_color_mode == MODE_RGB);
  gtk_widget_set_visible(GTK_WIDGET(g->channel_tabs_Y0U0V0),
                         p->wavelet_color_mode == MODE_Y0U0V0);
  if((p->wavelet_color_mode == MODE_Y0U0V0) && (g->channel < DT_DENOISE_PROFILE_Y0))
  {
    g->channel = DT_DENOISE_PROFILE_Y0;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g->channel_tabs_Y0U0V0),
                                  g->channel - DT_DENOISE_PROFILE_Y0);
  }
  if((p->wavelet_color_mode == MODE_RGB) && (g->channel > DT_DENOISE_PROFILE_B))
  {
    g->channel = DT_DENOISE_PROFILE_ALL;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g->channel_tabs), g->channel);
  }
}

void gui_reset(dt_iop_module_t *self)
{
  dt_iop_denoiseprofile_gui_data_t *g = (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;
  dt_iop_denoiseprofile_params_t *p = (dt_iop_denoiseprofile_params_t *)self->params;
  if(p->wavelet_color_mode == MODE_Y0U0V0)
  {
    g->channel = DT_DENOISE_PROFILE_Y0;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g->channel_tabs_Y0U0V0),
                                  g->channel - DT_DENOISE_PROFILE_Y0);
  }
  else
  {
    g->channel = DT_DENOISE_PROFILE_ALL;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g->channel_tabs), g->channel);
  }
  gtk_widget_set_visible(g->fix_anscombe_and_nlmeans_norm,
                         !p->fix_anscombe_and_nlmeans_norm);
  gtk_widget_set_visible(g->use_new_vst, !p->use_new_vst);
}

static void dt_iop_denoiseprofile_get_params(dt_iop_denoiseprofile_params_t *p,
                                             const int ch,
                                             const double mouse_x,
                                             const double mouse_y,
                                             const float rad)
{
  for(int k = 0; k < DT_IOP_DENOISE_PROFILE_BANDS; k++)
  {
    const float f = expf(-(mouse_x - p->x[ch][k]) * (mouse_x - p->x[ch][k]) / (rad * rad));
    p->y[ch][k] = (1 - f) * p->y[ch][k] + f * mouse_y;
  }
}

static gboolean denoiseprofile_draw_variance(GtkWidget *widget,
                                             cairo_t *crf,
                                             gpointer user_data)
{
  if(darktable.gui->reset) return FALSE;

  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_denoiseprofile_gui_data_t *c = (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;

  if(!isnan(c->variance_R))
  {
    gchar *str = g_strdup_printf("%.2f", c->variance_R);
    ++darktable.gui->reset;
    gtk_label_set_text(c->label_var_R, str);
    --darktable.gui->reset;
    g_free(str);
  }
  if(!isnan(c->variance_G))
  {
    gchar *str = g_strdup_printf("%.2f", c->variance_G);
    ++darktable.gui->reset;
    gtk_label_set_text(c->label_var_G, str);
    --darktable.gui->reset;
    g_free(str);
  }
  if(!isnan(c->variance_B))
  {
    gchar *str = g_strdup_printf("%.2f", c->variance_B);
    ++darktable.gui->reset;
    gtk_label_set_text(c->label_var_B, str);
    --darktable.gui->reset;
    g_free(str);
  }
  return FALSE;
}

static gboolean denoiseprofile_draw(GtkWidget *widget,
                                    cairo_t *crf,
                                    gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_denoiseprofile_gui_data_t *c = (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;
  dt_iop_denoiseprofile_params_t p = *(dt_iop_denoiseprofile_params_t *)self->params;

  int ch = (int)c->channel;
  dt_draw_curve_set_point(c->transition_curve, 0,
                          p.x[ch][DT_IOP_DENOISE_PROFILE_BANDS - 2] - 1.f, p.y[ch][0]);

  for(int k = 0; k < DT_IOP_DENOISE_PROFILE_BANDS; k++)
    dt_draw_curve_set_point(c->transition_curve, k + 1, p.x[ch][k], p.y[ch][k]);

  dt_draw_curve_set_point(c->transition_curve, DT_IOP_DENOISE_PROFILE_BANDS + 1,
                          p.x[ch][1] + 1.f,
                          p.y[ch][DT_IOP_DENOISE_PROFILE_BANDS - 1]);

  const int inset = DT_IOP_DENOISE_PROFILE_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  cairo_set_source_rgb(cr, .2, .2, .2);

  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2 * inset;
  height -= 2 * inset;

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
  cairo_set_source_rgb(cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);

  cairo_set_source_rgb(cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // draw grid
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.4));
  cairo_set_source_rgb(cr, .1, .1, .1);
  dt_draw_grid(cr, 8, 0, 0, width, height);

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max curves:
    dt_iop_denoiseprofile_get_params(&p, c->channel, c->mouse_x, 1., c->mouse_radius);
    dt_draw_curve_set_point(c->transition_curve, 0,
                            p.x[ch][DT_IOP_DENOISE_PROFILE_BANDS - 2] - 1.f, p.y[ch][0]);
    for(int k = 0; k < DT_IOP_DENOISE_PROFILE_BANDS; k++)
      dt_draw_curve_set_point(c->transition_curve, k + 1, p.x[ch][k], p.y[ch][k]);
    dt_draw_curve_set_point(c->transition_curve,
                            DT_IOP_DENOISE_PROFILE_BANDS + 1, p.x[ch][1] + 1.f,
                            p.y[ch][DT_IOP_DENOISE_PROFILE_BANDS - 1]);
    dt_draw_curve_calc_values(c->transition_curve,
                              0.0, 1.0, DT_IOP_DENOISE_PROFILE_RES, c->draw_min_xs,
                              c->draw_min_ys);

    p = *(dt_iop_denoiseprofile_params_t *)self->params;
    dt_iop_denoiseprofile_get_params(&p, c->channel, c->mouse_x, .0, c->mouse_radius);
    dt_draw_curve_set_point(c->transition_curve,
                            0, p.x[ch][DT_IOP_DENOISE_PROFILE_BANDS - 2] - 1.f, p.y[ch][0]);
    for(int k = 0; k < DT_IOP_DENOISE_PROFILE_BANDS; k++)
      dt_draw_curve_set_point(c->transition_curve, k + 1, p.x[ch][k], p.y[ch][k]);
    dt_draw_curve_set_point(c->transition_curve,
                            DT_IOP_DENOISE_PROFILE_BANDS + 1, p.x[ch][1] + 1.f,
                            p.y[ch][DT_IOP_DENOISE_PROFILE_BANDS - 1]);
    dt_draw_curve_calc_values(c->transition_curve,
                              0.0, 1.0, DT_IOP_DENOISE_PROFILE_RES, c->draw_max_xs,
                              c->draw_max_ys);
  }

  cairo_save(cr);

  // draw selected cursor
  cairo_translate(cr, 0, height);

  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));

  for(int i = 0; i < DT_DENOISE_PROFILE_NONE; i++)
  {
    // draw curves, selected last
    ch = ((int)c->channel + i + 1) % DT_DENOISE_PROFILE_NONE;
    float alpha = 0.3;
    if(i == DT_DENOISE_PROFILE_NONE - 1) alpha = 1.0;
    if(p.wavelet_color_mode == MODE_RGB)
    {
      switch(ch)
      {
        case DT_DENOISE_PROFILE_ALL:
          cairo_set_source_rgba(cr, .7, .7, .7, alpha);
          break;
        case DT_DENOISE_PROFILE_R:
            cairo_set_source_rgba(cr, .7, .1, .1, alpha);
          break;
        case DT_DENOISE_PROFILE_G:
          cairo_set_source_rgba(cr, .1, .7, .1, alpha);
          break;
        case DT_DENOISE_PROFILE_B:
          cairo_set_source_rgba(cr, .1, .1, .7, alpha);
          break;
        default:
          cairo_set_source_rgba(cr, 7, .7, .7, 0.0f);
          break;
      }
    }
    else
    {
      switch(ch)
      {
        case DT_DENOISE_PROFILE_Y0:
          cairo_set_source_rgba(cr, .7, .7, .7, alpha);
          break;
        case DT_DENOISE_PROFILE_U0V0:
          cairo_set_source_rgba(cr, .8, .4, .0, alpha);
          break;
        default:
          cairo_set_source_rgba(cr, .7, .7, .7, 0.0f);
          break;
      }
    }

    p = *(dt_iop_denoiseprofile_params_t *)self->params;
    dt_draw_curve_set_point(c->transition_curve, 0,
                            p.x[ch][DT_IOP_DENOISE_PROFILE_BANDS - 2] - 1.0f, p.y[ch][0]);
    for(int k = 0; k < DT_IOP_DENOISE_PROFILE_BANDS; k++)
      dt_draw_curve_set_point(c->transition_curve, k + 1, p.x[ch][k], p.y[ch][k]);
    dt_draw_curve_set_point(c->transition_curve,
                            DT_IOP_DENOISE_PROFILE_BANDS + 1, p.x[ch][1] + 1.0f,
                            p.y[ch][DT_IOP_DENOISE_PROFILE_BANDS - 1]);
    dt_draw_curve_calc_values(c->transition_curve, 0.0, 1.0,
                              DT_IOP_DENOISE_PROFILE_RES, c->draw_xs, c->draw_ys);
    cairo_move_to(cr, 0 * width / (float)(DT_IOP_DENOISE_PROFILE_RES - 1),
                  -height * c->draw_ys[0]);
    for(int k = 1; k < DT_IOP_DENOISE_PROFILE_RES; k++)
      cairo_line_to(cr, k * width / (float)(DT_IOP_DENOISE_PROFILE_RES - 1),
                    -height * c->draw_ys[k]);
    cairo_stroke(cr);
  }

  ch = c->channel;
  // draw dots on knots
  cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  for(int k = 0; k < DT_IOP_DENOISE_PROFILE_BANDS; k++)
  {
    cairo_arc(cr, width * p.x[ch][k], -height * p.y[ch][k],
              DT_PIXEL_APPLY_DPI(3.0), 0.0, 2.0 * M_PI);
    if(c->x_move == k)
      cairo_fill(cr);
    else
      cairo_stroke(cr);
  }

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max, if selected
    cairo_set_source_rgba(cr, .7, .7, .7, .6);
    cairo_move_to(cr, 0, -height * c->draw_min_ys[0]);
    for(int k = 1; k < DT_IOP_DENOISE_PROFILE_RES; k++)
      cairo_line_to(cr, k * width / (float)(DT_IOP_DENOISE_PROFILE_RES - 1),
                    -height * c->draw_min_ys[k]);
    for(int k = DT_IOP_DENOISE_PROFILE_RES - 1; k >= 0; k--)
      cairo_line_to(cr, k * width / (float)(DT_IOP_DENOISE_PROFILE_RES - 1),
                    -height * c->draw_max_ys[k]);
    cairo_close_path(cr);
    cairo_fill(cr);
    // draw mouse focus circle
    cairo_set_source_rgba(cr, .9, .9, .9, .5);
    const float pos = DT_IOP_DENOISE_PROFILE_RES * c->mouse_x;
    int k = (int)pos;
    const float f = k - pos;
    if(k >= DT_IOP_DENOISE_PROFILE_RES - 1) k = DT_IOP_DENOISE_PROFILE_RES - 2;
    float ht = -height * (f * c->draw_ys[k] + (1 - f) * c->draw_ys[k + 1]);
    cairo_arc(cr, c->mouse_x * width, ht, c->mouse_radius * width, 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  cairo_restore(cr);

  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

  // draw labels:
  PangoLayout *layout;
  PangoRectangle ink;
  PangoFontDescription *desc =
    pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  pango_font_description_set_absolute_size(desc, (.08 * height) * PANGO_SCALE);
  layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  cairo_set_source_rgb(cr, .1, .1, .1);

  pango_layout_set_text(layout, _("coarse"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .02 * width - ink.y, .5 * (height + ink.width));
  cairo_save(cr);
  cairo_rotate(cr, -M_PI * .5f);
  pango_cairo_show_layout(cr, layout);
  cairo_restore(cr);

  pango_layout_set_text(layout, _("fine"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .98 * width - ink.height, .5 * (height + ink.width));
  cairo_save(cr);
  cairo_rotate(cr, -M_PI * .5f);
  pango_cairo_show_layout(cr, layout);
  cairo_restore(cr);


  pango_layout_set_text(layout, _("smooth"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .5 * (width - ink.width), .08 * height - ink.height);
  pango_cairo_show_layout(cr, layout);

  pango_layout_set_text(layout, _("noisy"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .5 * (width - ink.width), .97 * height - ink.height);
  pango_cairo_show_layout(cr, layout);

  pango_font_description_free(desc);
  g_object_unref(layout);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean denoiseprofile_motion_notify(GtkWidget *widget,
                                             GdkEventMotion *event,
                                             gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_denoiseprofile_gui_data_t *c = (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;
  dt_iop_denoiseprofile_params_t *p = (dt_iop_denoiseprofile_params_t *)self->params;
  const int inset = DT_IOP_DENOISE_PROFILE_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
  if(!c->dragging) c->mouse_x = CLAMP(event->x - inset, 0, width) / (float)width;
  c->mouse_y = 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;
  if(c->dragging)
  {
    *p = c->drag_params;
    if(c->x_move < 0)
    {
      dt_iop_denoiseprofile_get_params(p, c->channel,
                                       c->mouse_x, c->mouse_y + c->mouse_pick,
                                       c->mouse_radius);
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else
  {
    c->x_move = -1;
  }
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean denoiseprofile_button_press(GtkWidget *widget,
                                            GdkEventButton *event,
                                            gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_denoiseprofile_gui_data_t *c = (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;
  const int ch = c->channel;
  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    // reset current curve
    dt_iop_denoiseprofile_params_t *p = (dt_iop_denoiseprofile_params_t *)self->params;
    dt_iop_denoiseprofile_params_t *d =
      (dt_iop_denoiseprofile_params_t *)self->default_params;

    for(int k = 0; k < DT_IOP_DENOISE_PROFILE_BANDS; k++)
    {
      p->x[ch][k] = d->x[ch][k];
      p->y[ch][k] = d->y[ch][k];
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(self->widget);
  }
  else if(event->button == 1)
  {
    c->drag_params = *(dt_iop_denoiseprofile_params_t *)self->params;
    const int inset = DT_IOP_DENOISE_PROFILE_INSET;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
    c->mouse_pick
        = dt_draw_curve_calc_value(c->transition_curve,
                                   CLAMP(event->x - inset, 0, width) / (float)width);
    c->mouse_pick -= 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;
    c->dragging = 1;
    return TRUE;
  }
  return FALSE;
}

static gboolean denoiseprofile_button_release(GtkWidget *widget,
                                              GdkEventButton *event,
                                              gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_denoiseprofile_gui_data_t *c =
      (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;
    c->dragging = 0;
    return TRUE;
  }
  return FALSE;
}

static gboolean denoiseprofile_leave_notify(GtkWidget *widget,
                                            GdkEventCrossing *event,
                                            gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_denoiseprofile_gui_data_t *c = (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;
  if(!c->dragging) c->mouse_y = -1.0;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean denoiseprofile_scrolled(GtkWidget *widget,
                                        GdkEventScroll *event,
                                        gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_denoiseprofile_gui_data_t *c = (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;

  if(dt_gui_ignore_scroll(event)) return FALSE;

  int delta_y;
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    c->mouse_radius = CLAMP(c->mouse_radius * (1.f + 0.1f * delta_y),
                            0.2f / DT_IOP_DENOISE_PROFILE_BANDS, 1.f);
    gtk_widget_queue_draw(widget);
  }

  return TRUE;
}

static void denoiseprofile_tab_switch(GtkNotebook *notebook,
                                      GtkWidget *page,
                                      const guint page_num,
                                      gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_denoiseprofile_params_t *p = (dt_iop_denoiseprofile_params_t *)self->params;
  if(darktable.gui->reset) return;
  dt_iop_denoiseprofile_gui_data_t *c = (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;
  if(p->wavelet_color_mode == MODE_Y0U0V0)
    c->channel = (dt_iop_denoiseprofile_channel_t)page_num + DT_DENOISE_PROFILE_Y0;
  else
    c->channel = (dt_iop_denoiseprofile_channel_t)page_num;
  gtk_widget_queue_draw(self->widget);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_denoiseprofile_gui_data_t *g = IOP_GUI_ALLOC(denoiseprofile);
  dt_iop_denoiseprofile_params_t *p =
    (dt_iop_denoiseprofile_params_t *)self->default_params;

  g->profiles = NULL;

  g->channel = 0;

  // First build sub-level boxes
  g->box_nlm = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->radius = dt_bauhaus_slider_from_params(self, "radius");
  dt_bauhaus_slider_set_soft_range(g->radius, 0.0, 8.0);
  dt_bauhaus_slider_set_digits(g->radius, 0);
  g->nbhood = dt_bauhaus_slider_from_params(self, "nbhood");
  dt_bauhaus_slider_set_digits(g->nbhood, 0);
  g->scattering = dt_bauhaus_slider_from_params(self, "scattering");
  dt_bauhaus_slider_set_soft_max(g->scattering, 1.0f);
  g->central_pixel_weight = dt_bauhaus_slider_from_params(self, "central_pixel_weight");
  dt_bauhaus_slider_set_soft_max(g->central_pixel_weight, 1.0f);

  g->box_wavelets = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->wavelet_color_mode = dt_bauhaus_combobox_from_params(self, "wavelet_color_mode");

  g->channel_tabs = GTK_NOTEBOOK(gtk_notebook_new());
  dt_action_define_iop(self, NULL, N_("channel"), GTK_WIDGET(g->channel_tabs),
                       &dt_action_def_tabs_rgb);
  dt_ui_notebook_page(g->channel_tabs, N_("all"), NULL);
  dt_ui_notebook_page(g->channel_tabs, N_("R"), NULL);
  dt_ui_notebook_page(g->channel_tabs, N_("G"), NULL);
  dt_ui_notebook_page(g->channel_tabs, N_("B"), NULL);
  g_signal_connect(G_OBJECT(g->channel_tabs), "switch_page",
                   G_CALLBACK(denoiseprofile_tab_switch), self);
  gtk_box_pack_start(GTK_BOX(g->box_wavelets),
                     GTK_WIDGET(g->channel_tabs), FALSE, FALSE, 0);

  g->channel_tabs_Y0U0V0 = GTK_NOTEBOOK(gtk_notebook_new());
  dt_ui_notebook_page(g->channel_tabs_Y0U0V0, N_("Y0"), NULL);
  dt_ui_notebook_page(g->channel_tabs_Y0U0V0, N_("U0V0"), NULL);
  g_signal_connect(G_OBJECT(g->channel_tabs_Y0U0V0), "switch_page",
                   G_CALLBACK(denoiseprofile_tab_switch), self);
  gtk_box_pack_start(GTK_BOX(g->box_wavelets),
                     GTK_WIDGET(g->channel_tabs_Y0U0V0), FALSE, FALSE, 0);

  const int ch = (int)g->channel;
  g->transition_curve = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
  (void)dt_draw_curve_add_point(g->transition_curve,
                                p->x[ch][DT_IOP_DENOISE_PROFILE_BANDS - 2] - 1.0f,
                                p->y[ch][DT_IOP_DENOISE_PROFILE_BANDS - 2]);
  for(int k = 0; k < DT_IOP_DENOISE_PROFILE_BANDS; k++)
    (void)dt_draw_curve_add_point(g->transition_curve, p->x[ch][k], p->y[ch][k]);
  (void)dt_draw_curve_add_point(g->transition_curve, p->x[ch][1] + 1.0f, p->y[ch][1]);

  g->mouse_x = g->mouse_y = g->mouse_pick = -1.0;
  g->dragging = 0;
  g->x_move = -1;
  g->mouse_radius = 1.0f / (DT_IOP_DENOISE_PROFILE_BANDS * 2);

  g->area = GTK_DRAWING_AREA
    (dt_ui_resize_wrap(NULL, 0,
                       "plugins/darkroom/denoiseprofile/aspect_percent"));
  dt_action_define_iop(self, NULL, N_("graph"), GTK_WIDGET(g->area), NULL);

  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(denoiseprofile_draw), self);
  g_signal_connect(G_OBJECT(g->area), "button-press-event",
                   G_CALLBACK(denoiseprofile_button_press), self);
  g_signal_connect(G_OBJECT(g->area), "button-release-event",
                   G_CALLBACK(denoiseprofile_button_release), self);
  g_signal_connect(G_OBJECT(g->area), "motion-notify-event",
                   G_CALLBACK(denoiseprofile_motion_notify), self);
  g_signal_connect(G_OBJECT(g->area), "leave-notify-event",
                   G_CALLBACK(denoiseprofile_leave_notify), self);
  g_signal_connect(G_OBJECT(g->area), "scroll-event",
                   G_CALLBACK(denoiseprofile_scrolled), self);
  gtk_box_pack_start(GTK_BOX(g->box_wavelets), GTK_WIDGET(g->area), FALSE, FALSE, 0);

  g->box_variance = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->label_var = GTK_LABEL(dt_ui_label_new(_("use only with a perfectly\n"
                                             "uniform image if you want to\n"
                                             "estimate the noise variance.")));
  gtk_box_pack_start(GTK_BOX(g->box_variance), GTK_WIDGET(g->label_var), TRUE, TRUE, 0);

  GtkBox *hboxR = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  GtkLabel *labelR = GTK_LABEL(dt_ui_label_new(_("variance red: ")));
  gtk_box_pack_start(GTK_BOX(hboxR), GTK_WIDGET(labelR), FALSE, FALSE, 0);
  g->label_var_R = GTK_LABEL(dt_ui_label_new("")); // This gets filled in by process
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->label_var_R),
                              _("variance computed on the red channel"));
  gtk_box_pack_start(GTK_BOX(hboxR), GTK_WIDGET(g->label_var_R), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(g->box_variance), GTK_WIDGET(hboxR), TRUE, TRUE, 0);

  GtkBox *hboxG = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  GtkLabel *labelG = GTK_LABEL(dt_ui_label_new(_("variance green: ")));
  gtk_box_pack_start(GTK_BOX(hboxG), GTK_WIDGET(labelG), FALSE, FALSE, 0);
  g->label_var_G = GTK_LABEL(dt_ui_label_new("")); // This gets filled in by process
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->label_var_G),
                              _("variance computed on the green channel"));
  gtk_box_pack_start(GTK_BOX(hboxG), GTK_WIDGET(g->label_var_G), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(g->box_variance), GTK_WIDGET(hboxG), TRUE, TRUE, 0);

  GtkBox *hboxB = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  GtkLabel *labelB = GTK_LABEL(dt_ui_label_new(_("variance blue: ")));
  gtk_box_pack_start(GTK_BOX(hboxB), GTK_WIDGET(labelB), FALSE, FALSE, 0);
  g->label_var_B = GTK_LABEL(dt_ui_label_new("")); // This gets filled in by process
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->label_var_B),
                              _("variance computed on the blue channel"));
  gtk_box_pack_start(GTK_BOX(hboxB), GTK_WIDGET(g->label_var_B), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(g->box_variance), GTK_WIDGET(hboxB), TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(g->box_variance), "draw",
                   G_CALLBACK(denoiseprofile_draw_variance), self);

  // start building top level widget
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->profile = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->profile, NULL, N_("profile"));
  g_signal_connect(G_OBJECT(g->profile), "value-changed",
                   G_CALLBACK(profile_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->profile, TRUE, TRUE, 0);

  g->wb_adaptive_anscombe = dt_bauhaus_toggle_from_params(self, "wb_adaptive_anscombe");

  g->mode = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->mode, NULL, N_("mode"));
  dt_bauhaus_combobox_add(g->mode, _("non-local means"));
  dt_bauhaus_combobox_add(g->mode, _("non-local means auto"));
  dt_bauhaus_combobox_add(g->mode, _("wavelets"));
  dt_bauhaus_combobox_add(g->mode, _("wavelets auto"));
  const gboolean compute_variance =
    dt_conf_get_bool("plugins/darkroom/denoiseprofile/show_compute_variance_mode");
  if(compute_variance) dt_bauhaus_combobox_add(g->mode, _("compute variance"));
  g_signal_connect(G_OBJECT(g->mode), "value-changed", G_CALLBACK(mode_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->mode, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), g->box_nlm, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->box_wavelets, TRUE, TRUE, 0);

  g->overshooting = dt_bauhaus_slider_from_params(self, "overshooting");
  dt_bauhaus_slider_set_soft_max(g->overshooting, 4.0f);
  g->strength = dt_bauhaus_slider_from_params(self, N_("strength"));
  dt_bauhaus_slider_set_soft_max(g->strength, 4.0f);
  dt_bauhaus_slider_set_digits(g->strength, 3);
  g->shadows = dt_bauhaus_slider_from_params(self, "shadows");
  g->bias = dt_bauhaus_slider_from_params(self, "bias");
  dt_bauhaus_slider_set_soft_range(g->bias, -10.0f, 10.0f);

  gtk_box_pack_start(GTK_BOX(self->widget), g->box_variance, TRUE, TRUE, 0);

  g->fix_anscombe_and_nlmeans_norm = dt_bauhaus_toggle_from_params
    (self, "fix_anscombe_and_nlmeans_norm");

  g->use_new_vst = dt_bauhaus_toggle_from_params(self, "use_new_vst");

  gtk_widget_set_tooltip_text(g->wb_adaptive_anscombe,
                              _("adapt denoising according to the\n"
                                "white balance coefficients.\n"
                                "should be enabled on a first instance\n"
                                "for better denoising.\n"
                                "should be disabled if an earlier instance\n"
                                "has been used with a color blending mode."));
  gtk_widget_set_tooltip_text(g->fix_anscombe_and_nlmeans_norm,
                              _("fix bugs in anscombe transform resulting\n"
                                "in undersmoothing of the green channel in\n"
                                "wavelets mode, combined with a bad handling\n"
                                "of white balance coefficients, and a bug in\n"
                                "non local means normalization resulting in\n"
                                "undersmoothing when patch size was increased.\n"
                                "enabling this option will change the denoising\n"
                                "you get. once enabled, you won't be able to\n"
                                "return back to old algorithm."));
  gtk_widget_set_tooltip_text(g->profile,
                              _("profile used for variance stabilization"));
  gtk_widget_set_tooltip_text(g->mode,
                              _("method used in the denoising core.\n"
                                "non-local means works best for `lightness' blending,\n"
                                "wavelets work best for `color' blending"));
  gtk_widget_set_tooltip_text(g->wavelet_color_mode,
                              _("color representation used within the algorithm.\n"
                                "RGB keeps the RGB channels separated,\n"
                                "while Y0U0V0 combine the channels to\n"
                                "denoise chroma and luma separately."));
  gtk_widget_set_tooltip_text(g->radius,
                              _("radius of the patches to match.\n"
                                "increase for more sharpness on strong edges,"
                                " and better denoising of smooth areas.\n"
                                "if details are oversmoothed, reduce this value or"
                                " increase the central pixel weight slider."));
  gtk_widget_set_tooltip_text(g->nbhood,
                              _("emergency use only: radius of the neighborhood to"
                                " search patches in. "
                                "increase for better denoising performance,"
                                " but watch the long runtimes! "
                                "large radii can be very slow. you have been warned"));
  gtk_widget_set_tooltip_text(g->scattering,
                              _("scattering of the neighborhood to search patches in.\n"
                                "increase for better coarse-grain noise reduction.\n"
                                "does not affect execution time."));
  gtk_widget_set_tooltip_text(g->central_pixel_weight,
                              _("increase the weight of the central pixel\n"
                                "of the patch in the patch comparison.\n"
                                "useful to recover details when patch size\n"
                                "is quite big."));
  gtk_widget_set_tooltip_text(g->strength, _("finetune denoising strength"));
  gtk_widget_set_tooltip_text(g->overshooting,
                              _("controls the way parameters are autoset\n"
                                "increase if shadows are not denoised enough\n"
                                "or if chroma noise remains.\n"
                                "this can happen if your picture is underexposed."));
  gtk_widget_set_tooltip_text(g->shadows,
                              _("finetune shadows denoising.\n"
                                "decrease to denoise more aggressively\n"
                                "dark areas of the image."));
  gtk_widget_set_tooltip_text(g->bias,
                              _("correct color cast in shadows.\n"
                                "decrease if shadows are too purple.\n"
                                "increase if shadows are too green."));
  gtk_widget_set_tooltip_text(g->use_new_vst,
                              _("upgrade the variance stabilizing algorithm.\n"
                                "new algorithm extends the current one.\n"
                                "it is more flexible but could give small\n"
                                "differences in the images already processed."));

}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_denoiseprofile_gui_data_t *g = (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;
  g_list_free_full(g->profiles, dt_noiseprofile_free);
  dt_draw_curve_destroy(g->transition_curve);
  // nothing else necessary, gtk will clean up the slider.

  IOP_GUI_FREE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
