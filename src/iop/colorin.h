/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.

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
#ifndef DARKTABLE_IOP_COLORIN_H
#define DARKTABLE_IOP_COLORIN_H

#include "common/colorspaces.h"
#include "develop/imageop.h"
#include <gtk/gtk.h>
#include <inttypes.h>

// max iccprofile file name length
#define DT_IOP_COLOR_ICC_LEN 100

#define LUT_SAMPLES 0x10000

// constants fit to the ones from lcms.h:
typedef enum dt_iop_color_intent_t
{
  DT_INTENT_PERCEPTUAL             = INTENT_PERCEPTUAL,            // 0
  DT_INTENT_RELATIVE_COLORIMETRIC  = INTENT_RELATIVE_COLORIMETRIC, // 1
  DT_INTENT_SATURATION             = INTENT_SATURATION,            // 2
  DT_INTENT_ABSOLUTE_COLORIMETRIC  = INTENT_ABSOLUTE_COLORIMETRIC  // 3
}
dt_iop_color_intent_t;

typedef enum dt_iop_color_normalize_t
{
  DT_NORMALIZE_OFF,
  DT_NORMALIZE_SRGB,
  DT_NORMALIZE_ADOBE_RGB,
  DT_NORMALIZE_LINEAR_RGB,
  DT_NORMALIZE_BETA_RGB
}
dt_iop_color_normalize_t;

typedef struct dt_iop_color_profile_t
{
  char filename[512]; // icc file name
  char name[512];     // product name
  int  pos;           // position in combo box
  int  display_pos;   // position in display combo box
}
dt_iop_color_profile_t;

typedef struct dt_iop_colorin_params1_t
{
  char iccprofile[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t intent;
}
dt_iop_colorin_params1_t;

typedef struct dt_iop_colorin_params_t
{
  char iccprofile[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t intent;
  int normalize;
}
dt_iop_colorin_params_t;

typedef struct dt_iop_colorin_gui_data_t
{
  GtkWidget *cbox1, *cbox2, *cbox3;
  GList *image_profiles, *global_profiles;
  int n_image_profiles;
}
dt_iop_colorin_gui_data_t;

typedef struct dt_iop_colorin_global_data_t
{
  int kernel_colorin_unbound;
  int kernel_colorin_clipping;
}
dt_iop_colorin_global_data_t;

typedef struct dt_iop_colorin_data_t
{
  cmsHPROFILE input;
  cmsHPROFILE Lab;
  cmsHPROFILE nrgb;
  cmsHTRANSFORM *xform_cam_Lab;
  cmsHTRANSFORM *xform_cam_nrgb;
  cmsHTRANSFORM *xform_nrgb_Lab;
  float lut[3][LUT_SAMPLES];
  float cmatrix[9];
  float nmatrix[9];
  float lmatrix[9];
  float unbounded_coeffs[3][3];       // approximation for extrapolation of shaper curves
}
dt_iop_colorin_data_t;

void init(dt_iop_module_t *module);
void cleanup(dt_iop_module_t *module);

void gui_update    (struct dt_iop_module_t *self);
void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void reset_params  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);

void gui_init     (struct dt_iop_module_t *self);
void gui_cleanup  (struct dt_iop_module_t *self);

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
