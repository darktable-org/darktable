/*
    This file is part of darktable,
    Copyright (C) 2011-2024 darktable developers.

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
#include "common/debug.h"
#include "common/imagebuf.h"
#include "common/opencl.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/borders_helper.h"
#include "develop/imageop_gui.h"
#include "dtgtk/button.h"
#include "dtgtk/resetlabel.h"
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(4, dt_iop_borders_params_t)

// Module constants
#define DT_IOP_BORDERS_ASPECT_IMAGE_VALUE 0.0f
#define DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE -1.0f
typedef enum dt_iop_orientation_t
{
  DT_IOP_BORDERS_ASPECT_ORIENTATION_AUTO = 0,      // $DESCRIPTION: "auto"
  DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT = 1,  // $DESCRIPTION: "portrait"
  DT_IOP_BORDERS_ASPECT_ORIENTATION_LANDSCAPE = 2, // $DESCRIPTION: "landscape"
} dt_iop_orientation_t;

typedef enum dt_iop_basis_t
{
  DT_IOP_BORDERS_BASIS_AUTO = 0,     // $DESCRIPTION: "auto"
  DT_IOP_BORDERS_BASIS_WIDTH = 1,    // $DESCRIPTION: "width"
  DT_IOP_BORDERS_BASIS_HEIGHT = 2,   // $DESCRIPTION: "height"
  DT_IOP_BORDERS_BASIS_SHORTER = 3,  // $DESCRIPTION: "shorter"
  DT_IOP_BORDERS_BASIS_LONGER = 4,   // $DESCRIPTION: "longer"
} dt_iop_basis_t;

static const float _aspect_ratios[]
  = { DT_IOP_BORDERS_ASPECT_IMAGE_VALUE,
      3.0f, 95.0f / 33.0f, 2.39f, 2.0f, 16.0f / 9.0f, 5.0f / 3.0f, 14.0f / 8.5f, PHI, 16.0f / 10.0f,
      3.0f / 2.0f, 297.0f / 210.0f, M_SQRT2, 7.0f / 5.0f, 4.0f / 3.0f, 11.0f / 8.5f, 14.0f / 11.0f,
      5.0f / 4.0f, 1.0f, DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE };
static const float _pos_h_ratios[] = { 0.5f, 1.0f / 3.0f, 3.0f / 8.0f, 5.0f / 8.0f, 2.0f / 3.0f };
static const float _pos_v_ratios[] = { 0.5f, 1.0f / 3.0f, 3.0f / 8.0f, 5.0f / 8.0f, 2.0f / 3.0f };

#define DT_IOP_BORDERS_ASPECT_COUNT G_N_ELEMENTS(_aspect_ratios)
#define DT_IOP_BORDERS_ASPECT_IMAGE_IDX 0
#define DT_IOP_BORDERS_ASPECT_CONSTANT_IDX (DT_IOP_BORDERS_ASPECT_COUNT - 1)
#define DT_IOP_BORDERS_POSITION_H_COUNT G_N_ELEMENTS(_pos_h_ratios)
#define DT_IOP_BORDERS_POSITION_V_COUNT G_N_ELEMENTS(_pos_v_ratios)

typedef struct dt_iop_borders_params_t
{
  float color[3];           // border color $DEFAULT: 1.0 $DESCRIPTION: "border color"
  float aspect;             /* aspect ratio of the outer frame w/h
                               $MIN: 1.0 $MAX: 3.0 $DEFAULT: DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE $DESCRIPTION: "aspect ratio" */
  char aspect_text[20];     /* UNUSED aspect ratio of the outer frame w/h (user string version)
                               DEFAULT: "constant border" */
  dt_iop_orientation_t aspect_orient;        /* aspect ratio orientation
                               $DEFAULT: 0 $DESCRIPTION: "orientation" */
  float size;               /* border width relative to the length of the chosen basis
                               $MIN: 0.0 $MAX: 0.5 $DEFAULT: 0.1 $DESCRIPTION: "border size" */
  float pos_h;              /* picture horizontal position ratio into the final image
                               $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "horizontal offset" */
  char pos_h_text[20];      /* UNUSED picture horizontal position ratio into the final image (user string version)
                               DEFAULT: "1/2" */
  float pos_v;              /* picture vertical position ratio into the final image
                               $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "vertical offset"*/
  char pos_v_text[20];      /* UNUSED picture vertical position ratio into the final image (user string version)
                               DEFAULT: "1/2" */
  float frame_size;         /* frame line width relative to border width
                               $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "frame line size" */
  float frame_offset;       /* frame offset from picture size relative to [border width - frame width]
                               $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "frame line offset" */
  float frame_color[3];     // frame line color $DEFAULT: 0.0 $DESCRIPTION: "frame line color"
  gboolean max_border_size; /* the way border size is computed
                               $DEFAULT: TRUE */
  dt_iop_basis_t basis;     /* side of the photo to use as basis for the size calculation
                               $DEFAULT: 0 $DESCRIPTION: "basis" */
} dt_iop_borders_params_t;

typedef struct dt_iop_borders_gui_data_t
{
  GtkWidget *basis;
  GtkWidget *size;
  GtkWidget *aspect;
  GtkWidget *aspect_slider;
  GtkWidget *aspect_orient;
  GtkWidget *pos_h;
  GtkWidget *pos_h_slider;
  GtkWidget *pos_v;
  GtkWidget *pos_v_slider;
  GtkWidget *colorpick;
  GtkWidget *border_picker; // the 1st button
  GtkWidget *frame_size;
  GtkWidget *frame_offset;
  GtkWidget *frame_colorpick;
  GtkWidget *frame_picker; // the 2nd button
} dt_iop_borders_gui_data_t;

// ******* Check and update legacy params...(esp. ver 4)
int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_borders_params_v3_t
  {
    float color[3];           // border color $DEFAULT: 1.0
    float aspect;             /* aspect ratio of the outer frame w/h
                               $MIN: 1.0 $MAX: 3.0 $DEFAULT: DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE $DESCRIPTION: "aspect ratio" */
    char aspect_text[20];     /* UNUSED aspect ratio of the outer frame w/h (user string version)
                                 DEFAULT: "constant border" */
    dt_iop_orientation_t aspect_orient;        /* aspect ratio orientation
                                                  $DEFAULT: 0 $DESCRIPTION: "orientation" */
    float size;               /* border width relative to overall frame width
                                 $MIN: 0.0 $MAX: 0.5 $DEFAULT: 0.1 $DESCRIPTION: "border size" */
    float pos_h;              /* picture horizontal position ratio into the final image
                                 $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "horizontal offset" */
    char pos_h_text[20];      /* UNUSED picture horizontal position ratio into the final image (user string version)
                                 DEFAULT: "1/2" */
    float pos_v;              /* picture vertical position ratio into the final image
                                 $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "vertical offset"*/
    char pos_v_text[20];      /* UNUSED picture vertical position ratio into the final image (user string version)
                                 DEFAULT: "1/2" */
    float frame_size;         /* frame line width relative to border width
                                 $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "frame line size" */
    float frame_offset;       /* frame offset from picture size relative to [border width - frame width]
                                 $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "frame line offset" */
    float frame_color[3];     // frame line color $DEFAULT: 0.0
    gboolean max_border_size; /* the way border size is computed
                                 $DEFAULT: TRUE */
  } dt_iop_borders_params_v3_t;

  dt_iop_borders_params_v3_t default_v3 =
    { { 1.0f, 1.0f, 1.0f },
      DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE,
      "                   ",
      0,
      0.1f,
      0.5f,
      "                   ",
      0.5f,
      "                   ",
      0.0f,
      0.5f,
      { 0.0f, 0.0f, 0.0f },
      TRUE
    };

  if(old_version == 1)
  {
    typedef struct dt_iop_borders_params_v1_t
    {
      float color[3]; // border color
      float aspect;   // aspect ratio of the outer frame w/h
      float size;     // border width relative to overall frame width
    } dt_iop_borders_params_v1_t;

    const dt_iop_borders_params_v1_t *o = (dt_iop_borders_params_v1_t *)old_params;
    dt_iop_borders_params_v3_t *n = malloc(sizeof(dt_iop_borders_params_v3_t));

    *n = default_v3; // start with a fresh copy of default parameters
    memcpy(n->color, o->color, sizeof(o->color));
    n->aspect = (o->aspect < 1) ? 1 / o->aspect : o->aspect;
    // no auto orientation in legacy param due to already convert aspect ratio
    n->aspect_orient = o->aspect > 1 ? DT_IOP_BORDERS_ASPECT_ORIENTATION_LANDSCAPE
                                     : DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT;
    n->size = fabsf(o->size); // no negative size any more (was for "constant border" detect)
    n->max_border_size = FALSE;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_borders_params_v3_t);
    *new_version = 3;
    return 0;
  }

  if(old_version == 2)
  {
    typedef struct dt_iop_borders_params_v2_t
    {
      float color[3];       // border color
      float aspect;         // aspect ratio of the outer frame w/h
      char aspect_text[20]; // aspect ratio of the outer frame w/h (user string version)
      int aspect_orient;    // aspect ratio orientation
      float size;           // border width relative to overall frame width
      float pos_h;          // picture horizontal position ratio into the final image
      char pos_h_text[20];  // picture horizontal position ratio into the final image (user string version)
      float pos_v;          // picture vertical position ratio into the final image
      char pos_v_text[20];  // picture vertical position ratio into the final image (user string version)
      float frame_size;     // frame line width relative to border width
      float frame_offset;   // frame offset from picture size relative to [border width - frame width]
      float frame_color[3]; // frame line color
    } dt_iop_borders_params_v2_t;

    const dt_iop_borders_params_v2_t *o = (dt_iop_borders_params_v2_t *)old_params;
    dt_iop_borders_params_v3_t *n = malloc(sizeof(dt_iop_borders_params_v3_t));

    memcpy(n, o, sizeof(struct dt_iop_borders_params_v2_t));
    n->max_border_size = FALSE;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_borders_params_v3_t);
    *new_version = 3;
    return 0;
  }

  if(old_version == 3)
  {
    typedef struct dt_iop_borders_params_v4_t
    {
      float color[3];           // border color $DEFAULT: 1.0
      float aspect;             /* aspect ratio of the outer frame w/h
                                $MIN: 1.0 $MAX: 3.0 $DEFAULT: DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE $DESCRIPTION: "aspect ratio" */
      char aspect_text[20];     /* UNUSED aspect ratio of the outer frame w/h (user string version)
                                  DEFAULT: "constant border" */
      dt_iop_orientation_t aspect_orient;        /* aspect ratio orientation
                                                    $DEFAULT: 0 $DESCRIPTION: "orientation" */
      float size;               /* border width relative to overall frame width
                                  $MIN: 0.0 $MAX: 0.5 $DEFAULT: 0.1 $DESCRIPTION: "border size" */
      float pos_h;              /* picture horizontal position ratio into the final image
                                  $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "horizontal offset" */
      char pos_h_text[20];      /* UNUSED picture horizontal position ratio into the final image (user string version)
                                  DEFAULT: "1/2" */
      float pos_v;              /* picture vertical position ratio into the final image
                                  $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "vertical offset"*/
      char pos_v_text[20];      /* UNUSED picture vertical position ratio into the final image (user string version)
                                  DEFAULT: "1/2" */
      float frame_size;         /* frame line width relative to border width
                                  $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "frame line size" */
      float frame_offset;       /* frame offset from picture size relative to [border width - frame width]
                                  $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "frame line offset" */
      float frame_color[3];     // frame line color $DEFAULT: 0.0
      gboolean max_border_size; /* the way border size is computed
                                  $DEFAULT: TRUE */
      dt_iop_basis_t basis;     /* side of the photo to use as basis for the size calculation
                                  $DEFAULT: 0 $DESCRIPTION: "basis" */
    } dt_iop_borders_params_v4_t;

    const dt_iop_borders_params_v3_t *o = (dt_iop_borders_params_v3_t *)old_params;
    dt_iop_borders_params_v4_t *n = malloc(sizeof(dt_iop_borders_params_v4_t));

    memcpy(n, o, sizeof(struct dt_iop_borders_params_v3_t));

    if (n->aspect == DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE && !n->max_border_size)
    {
      // the legacy behaviour is, when a constant border is used and the
      // max_border_size flag is set, the width is always used as basis.
      n->basis = DT_IOP_BORDERS_BASIS_WIDTH;
    }
    else
    {
      n->basis = DT_IOP_BORDERS_BASIS_AUTO;
    }

    *new_params = n;
    *new_params_size = sizeof(dt_iop_borders_params_v4_t);
    *new_version = 4;
    return 0;
  }

  return 1;
}

typedef struct dt_iop_borders_global_data_t
{
  int kernel_borders_fill;
} dt_iop_borders_global_data_t;


typedef struct dt_iop_borders_params_t dt_iop_borders_data_t;


const char *name()
{
  return _("framing");
}

const char *aliases()
{
  return _("borders|enlarge canvas|expand canvas");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("add solid borders or margins around the image"),
                                      _("creative"),
                                      _("linear or non-linear, RGB, display-referred"),
                                      _("geometric, RGB"),
                                      _("linear or non-linear, RGB, display-referred"));
}


int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_EFFECTS;
}

int operation_tags()
{
  return IOP_TAG_DISTORT | IOP_TAG_DECORATION;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_GUIDES_WIDGET | IOP_FLAGS_EXPAND_ROI_IN;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

gboolean distort_transform(dt_iop_module_t *self,
                           dt_dev_pixelpipe_iop_t *piece,
                           float *const restrict points,
                           size_t points_count)
{
  dt_iop_borders_data_t *d = piece->data;

  const int border_tot_width = (piece->buf_out.width - piece->buf_in.width);
  const int border_tot_height = (piece->buf_out.height - piece->buf_in.height);
  const int border_size_t = border_tot_height * d->pos_v;
  const int border_size_l = border_tot_width * d->pos_h;

  // nothing to be done if parameters are set to neutral values (no top/left border)
  if(border_size_l == 0 && border_size_t == 0) return TRUE;

  float *const pts = DT_IS_ALIGNED(points);

  DT_OMP_FOR(if(points_count > 100))
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    pts[i] += border_size_l;
    pts[i + 1] += border_size_t;
  }

  return TRUE;
}

gboolean distort_backtransform(dt_iop_module_t *self,
                               dt_dev_pixelpipe_iop_t *piece,
                               float *const restrict points,
                               size_t points_count)
{
  dt_iop_borders_data_t *d = piece->data;

  const int border_tot_width = (piece->buf_out.width - piece->buf_in.width);
  const int border_tot_height = (piece->buf_out.height - piece->buf_in.height);
  const int border_size_t = border_tot_height * d->pos_v;
  const int border_size_l = border_tot_width * d->pos_h;

  // nothing to be done if parameters are set to neutral values (no top/left border)
  if(border_size_l == 0 && border_size_t == 0) return TRUE;

  float *const pts = DT_IS_ALIGNED(points);
  DT_OMP_FOR(if(points_count > 100))
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    pts[i] -= border_size_l;
    pts[i + 1] -= border_size_t;
  }

  return TRUE;
}

void distort_mask(dt_iop_module_t *self,
                  dt_dev_pixelpipe_iop_t *piece,
                  const float *const in,
                  float *const out,
                  const dt_iop_roi_t *const roi_in,
                  const dt_iop_roi_t *const roi_out)
{
  dt_iop_borders_data_t *d = piece->data;

  dt_iop_border_positions_t binfo;

  dt_iop_setup_binfo(piece, roi_in, roi_out, d->pos_v, d->pos_h,
                     d->color, d->frame_color, d->frame_size, d->frame_offset, &binfo);

  const int border_in_x = binfo.border_in_x;
  const int border_in_y = binfo.border_in_y;

  // fill the image with 0 so that the added border isn't part of the mask
  dt_iop_image_fill(out, 0.0f, roi_out->width, roi_out->height, 1);

  // blit image inside border and fill the output with previous processed out
  DT_OMP_FOR()
  for(int j = 0; j < roi_in->height; j++)
  {
    float *outb = out + (size_t)(j + border_in_y) * roi_out->width + border_in_x;
    const float *inb = in + (size_t)j * roi_in->width;
    memcpy(outb, inb, sizeof(float) * roi_in->width);
  }
}

void modify_roi_out(dt_iop_module_t *self,
                    dt_dev_pixelpipe_iop_t *piece,
                    dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
  dt_iop_borders_data_t *d = piece->data;

  const float size = fabsf(d->size);

  const gboolean is_constant_border = d->aspect == DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE;

  dt_iop_basis_t basis = d->basis;
  if (basis == DT_IOP_BORDERS_BASIS_AUTO)
  {
    // automatic/legacy/default behaviour:
    // for a constant border be sure to base the computation on the
    // larger border, failing that the border will have a different
    // size depending on the orientation.
    // for all other borders use the width.
    basis = is_constant_border
      ? DT_IOP_BORDERS_BASIS_LONGER : DT_IOP_BORDERS_BASIS_WIDTH;
  }
  if(basis == DT_IOP_BORDERS_BASIS_LONGER)
  {
    basis = roi_in->width > roi_in->height
      ? DT_IOP_BORDERS_BASIS_WIDTH : DT_IOP_BORDERS_BASIS_HEIGHT;
  }
  else if(basis == DT_IOP_BORDERS_BASIS_SHORTER)
  {
    basis = roi_in->width < roi_in->height
      ? DT_IOP_BORDERS_BASIS_WIDTH : DT_IOP_BORDERS_BASIS_HEIGHT;
  }

  assert(basis == DT_IOP_BORDERS_BASIS_WIDTH
         || basis == DT_IOP_BORDERS_BASIS_HEIGHT);

  const int *basis_in = NULL, *other_in = NULL;
  int *basis_out = NULL, *other_out = NULL;

  #define DT_IOP_BORDERS_ASSIGN(b, b_uc, o) \
    basis_in = &roi_in->b, basis_out = &roi_out->b, \
    other_in = &roi_in->o, other_out = &roi_out->o, \
    basis = DT_IOP_BORDERS_BASIS_ ## b_uc
  #define DT_IOP_BORDERS_ASSIGN_width DT_IOP_BORDERS_ASSIGN(width, WIDTH, height)
  #define DT_IOP_BORDERS_ASSIGN_height DT_IOP_BORDERS_ASSIGN(height, HEIGHT, width)
  #define DT_IOP_BORDERS_ASSIGN_BASIS(basis) DT_IOP_BORDERS_ASSIGN_ ## basis

  if(basis == DT_IOP_BORDERS_BASIS_WIDTH)
    DT_IOP_BORDERS_ASSIGN_BASIS(width);
  else if(basis == DT_IOP_BORDERS_BASIS_HEIGHT)
    DT_IOP_BORDERS_ASSIGN_BASIS(height);

  if(is_constant_border)
  {
    *basis_out = roundf((float)*basis_in / (1.0f - size));
    *other_out = *other_in + *basis_out - *basis_in;
  }
  else
  {
    const float image_aspect = (float)roi_in->width / (float)(roi_in->height);

    float aspect = (d->aspect == DT_IOP_BORDERS_ASPECT_IMAGE_VALUE)
      ? image_aspect
      : d->aspect;

    if(d->aspect_orient == DT_IOP_BORDERS_ASPECT_ORIENTATION_AUTO)
      aspect = ((image_aspect < 1.0f && aspect > 1.0f)
                || (image_aspect > 1.0f && aspect < 1.0f))
        ? 1.0f / aspect
        : aspect;
    else if(d->aspect_orient == DT_IOP_BORDERS_ASPECT_ORIENTATION_LANDSCAPE)
      aspect = (aspect < 1.0f) ? 1.0f / aspect : aspect;
    else if(d->aspect_orient == DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT)
      aspect = (aspect > 1.0f) ? 1.0f / aspect : aspect;

    // first determine how large the border should be,
    float border_width = (float)*basis_in * (1.0f / (1.0f - size) - 1.0f);

    // then make sure we add that amount to the shorter side,
    if (basis == DT_IOP_BORDERS_BASIS_WIDTH && image_aspect < 1.0f)
      DT_IOP_BORDERS_ASSIGN_BASIS(height);
    else if (basis == DT_IOP_BORDERS_BASIS_HEIGHT && image_aspect > 1.0f)
      DT_IOP_BORDERS_ASSIGN_BASIS(width);

    // but add it to the longer side instead,
    // if the selected aspect ratio would cut off the image.
    if (basis == DT_IOP_BORDERS_BASIS_WIDTH && image_aspect < aspect)
      DT_IOP_BORDERS_ASSIGN_BASIS(height);
    else if (basis == DT_IOP_BORDERS_BASIS_HEIGHT && image_aspect > aspect)
      DT_IOP_BORDERS_ASSIGN_BASIS(width);

    if (basis == DT_IOP_BORDERS_BASIS_HEIGHT)
      aspect = 1.0f / aspect;

    *basis_out = roundf((float)*basis_in + border_width);
    *other_out = roundf((float)*basis_out / aspect);
  }

  // sanity check.
  const size_t max_dim = MAX(roi_in->width, roi_in->height);
  roi_out->width = CLAMP(roi_out->width, 1, 3 * max_dim);
  roi_out->height = CLAMP(roi_out->height, 1, 3 * max_dim);
}

void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out,
                   dt_iop_roi_t *roi_in)
{
  dt_iop_borders_data_t *d = piece->data;
  *roi_in = *roi_out;

  const float bw = (piece->buf_out.width - piece->buf_in.width) * roi_out->scale;
  const float bh = (piece->buf_out.height - piece->buf_in.height) * roi_out->scale;

  // don't request outside image (no px for borders)
  roi_in->x = MAX(roundf(roi_out->x - bw * d->pos_h), 0);
  roi_in->y = MAX(roundf(roi_out->y - bh * d->pos_v), 0);

  // subtract upper left border from dimensions
  roi_in->width -= MAX(roundf(bw * d->pos_h - roi_out->x), 0);
  roi_in->height -= MAX(roundf(bh * d->pos_v - roi_out->y), 0);

  // subtract lower right border from dimensions
  const float p_inw = (float)piece->buf_in.width * roi_out->scale;
  const float p_inh = (float)piece->buf_in.height * roi_out->scale;

  roi_in->width  -= MAX(roundf((float)(roi_in->x + roi_in->width) - p_inw), 0);
  roi_in->height -= MAX(roundf((float)(roi_in->y + roi_in->height) - p_inh), 0);

  // sanity check: don't request nothing or outside roi
  roi_in->width = MIN(p_inw, MAX(1, roi_in->width));
  roi_in->height = MIN(p_inh, MAX(1, roi_in->height));

  // FIXME: clamping to 1 leads to a one-pixel visual glitch if the
  // right/bottom border completely fills the viewport, but
  // changing it to 0 breaks all of the tiling_callback functions with
  // a division by zero.
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_borders_data_t *const d = piece->data;

  dt_iop_border_positions_t binfo;

  dt_iop_setup_binfo(piece, roi_in, roi_out, d->pos_v, d->pos_h,
                     d->color, d->frame_color, d->frame_size, d->frame_offset, &binfo);

  dt_iop_copy_image_with_border((float*)ovoid, (const float*)ivoid, &binfo);
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_borders_data_t *d = piece->data;
  dt_iop_borders_global_data_t *gd = self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;

  dt_iop_border_positions_t binfo;

  dt_iop_setup_binfo(piece, roi_in, roi_out, d->pos_v, d->pos_h,
                     d->color, d->frame_color, d->frame_size, d->frame_offset, &binfo);

  const int width = roi_out->width;
  const int height = roi_out->height;

  // ----- Filling border
  const float col[4] = { d->color[0], d->color[1], d->color[2], 1.0f };
  const int zero = 0;
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_borders_fill, width, height,
                            CLARG(dev_out), CLARG(zero), CLARG(zero),
                            CLARG(width), CLARG(height), CLARG(col));
  if(err != CL_SUCCESS) goto error;

  if(binfo.frame_size != 0)
  {
    const float col_frame[4] = { d->frame_color[0],
                                 d->frame_color[1],
                                 d->frame_color[2], 1.0f };

    const int roi_frame_in_width   = binfo.frame_br_in_x - binfo.frame_tl_in_x;
    const int roi_frame_in_height  = binfo.frame_br_in_y - binfo.frame_tl_in_y;
    const int roi_frame_out_width  = binfo.frame_br_out_x - binfo.frame_tl_out_x;
    const int roi_frame_out_height = binfo.frame_br_out_y - binfo.frame_tl_out_y;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_borders_fill, width, height,
                              CLARG(dev_out),
                              CLARG(binfo.frame_tl_out_x), CLARG(binfo.frame_tl_out_y),
                              CLARG(roi_frame_out_width), CLARG(roi_frame_out_height),
                              CLARG(col_frame));
    if(err != CL_SUCCESS) goto error;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_borders_fill, width, height,
                              CLARG(dev_out),
                              CLARG(binfo.frame_tl_in_x), CLARG(binfo.frame_tl_in_y),
                              CLARG(roi_frame_in_width), CLARG(roi_frame_in_height),
                              CLARG(col));
    if(err != CL_SUCCESS) goto error;
  }

  size_t iorigin[] = { 0, 0, 0 };
  size_t oorigin[] = { binfo.border_in_x, binfo.border_in_y, 0 };
  size_t region[]  = { roi_in->width, roi_in->height, 1 };

  // copy original input from dev_in -> dev_out as starting point
  err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, iorigin, oorigin, region);

error:
  return err;
}
#endif


void init_global(dt_iop_module_so_t *self)
{
  const int program = 2; // basic.cl from programs.conf

  dt_iop_borders_global_data_t *gd = malloc(sizeof(dt_iop_borders_global_data_t));
  self->data = gd;
  gd->kernel_borders_fill = dt_opencl_create_kernel(program, "borders_fill");
}


void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_borders_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_borders_fill);
  free(self->data);
  self->data = NULL;
}


void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)p1;
  dt_iop_borders_data_t *d = piece->data;
  memcpy(d, p, sizeof(dt_iop_borders_params_t));
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_borders_data_t));
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_borders_params_t p = (dt_iop_borders_params_t){ { 1.0f, 1.0f, 1.0f },
                                                         3.0f / 2.0f,
                                                         "3:2",
                                                         0,
                                                         0.1f,
                                                         0.5f,
                                                         "1/2",
                                                         0.5f,
                                                         "1/2",
                                                         0.0f,
                                                         0.5f,
                                                         { 0.0f, 0.0f, 0.0f },
                                                         TRUE,
                                                         DT_IOP_BORDERS_BASIS_AUTO };
  dt_gui_presets_add_generic(_("15:10 postcard white"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_NONE);

  p.color[0] = p.color[1] = p.color[2] = 0.0f;
  p.frame_color[0] = p.frame_color[1] = p.frame_color[2] = 1.0f;
  dt_gui_presets_add_generic(_("15:10 postcard black"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_NONE);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker,
                        dt_dev_pixelpipe_t *pipe)
{
  dt_iop_borders_gui_data_t *g = self->gui_data;
  dt_iop_borders_params_t *p = self->params;

  if(fabsf(p->color[0] - self->picked_color[0]) < 0.0001f
     && fabsf(p->color[1] - self->picked_color[1]) < 0.0001f
     && fabsf(p->color[2] - self->picked_color[2]) < 0.0001f)
  {
    // interrupt infinite loops
    return;
  }

  if(fabsf(p->frame_color[0] - self->picked_color[0]) < 0.0001f
     && fabsf(p->frame_color[1] - self->picked_color[1]) < 0.0001f
     && fabsf(p->frame_color[2] - self->picked_color[2]) < 0.0001f)
  {
    // interrupt infinite loops
    return;
  }

  GdkRGBA c = (GdkRGBA){.red = self->picked_color[0],
                        .green = self->picked_color[1],
                        .blue = self->picked_color[2],
                        .alpha = 1.0 };

  if(picker == g->frame_picker)
  {
    p->frame_color[0] = self->picked_color[0];
    p->frame_color[1] = self->picked_color[1];
    p->frame_color[2] = self->picked_color[2];
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->frame_colorpick), &c);
  }
  else if(picker == g->border_picker)
  {
    p->color[0] = self->picked_color[0];
    p->color[1] = self->picked_color[1];
    p->color[2] = self->picked_color[2];
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->colorpick), &c);
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _aspect_changed(GtkWidget *combo,
                            dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = self->gui_data;
  dt_iop_borders_params_t *p = self->params;
  const int which = dt_bauhaus_combobox_get(combo);
  if(which < DT_IOP_BORDERS_ASPECT_COUNT)
  {
    p->aspect = _aspect_ratios[which];
    ++darktable.gui->reset;
    dt_bauhaus_slider_set(g->aspect_slider,p->aspect);
    --darktable.gui->reset;
  }
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _position_h_changed(GtkWidget *combo,
                                dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = self->gui_data;
  dt_iop_borders_params_t *p = self->params;
  const int which = dt_bauhaus_combobox_get(combo);
  if(which < DT_IOP_BORDERS_POSITION_H_COUNT)
  {
    p->pos_h = _pos_h_ratios[which];
    ++darktable.gui->reset;
    dt_bauhaus_slider_set(g->pos_h_slider,p->pos_h);
    --darktable.gui->reset;
  }
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _position_v_changed(GtkWidget *combo,
                                dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = self->gui_data;
  dt_iop_borders_params_t *p = self->params;
  const int which = dt_bauhaus_combobox_get(combo);
  if(which < DT_IOP_BORDERS_POSITION_V_COUNT)
  {
    p->pos_v = _pos_v_ratios[which];
    ++darktable.gui->reset;
    dt_bauhaus_slider_set(g->pos_v_slider,p->pos_v);
    --darktable.gui->reset;
  }
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_changed(dt_iop_module_t *self,
                 GtkWidget *w,
                 void *previous)
{
  dt_iop_borders_gui_data_t *g = self->gui_data;
  dt_iop_borders_params_t *p = self->params;

  int k;
  if(!w || w == g->aspect_slider)
  {
    for(k = 0; k < DT_IOP_BORDERS_ASPECT_COUNT; k++)
    {
      if(fabsf(p->aspect - _aspect_ratios[k]) < 0.01f)
        break;
    }
    dt_bauhaus_combobox_set(g->aspect, k);
  }
  if(!w || w == g->pos_h_slider)
  {
    for(k = 0; k < DT_IOP_BORDERS_POSITION_H_COUNT; k++)
    {
      if(fabsf(p->pos_h - _pos_h_ratios[k]) < 0.01f)
        break;
    }
    dt_bauhaus_combobox_set(g->pos_h, k);
  }
  if(!w || w == g->pos_v_slider)
  {
    for(k = 0; k < DT_IOP_BORDERS_POSITION_V_COUNT; k++)
    {
      if(fabsf(p->pos_v - _pos_v_ratios[k]) < 0.01f)
        break;
    }
    dt_bauhaus_combobox_set(g->pos_v, k);
  }
}

static void _colorpick_color_set(GtkColorButton *widget,
                                dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_borders_params_t *p = self->params;

  // turn off the other color picker so that this tool actually works ...
  dt_iop_color_picker_reset(self, TRUE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  p->color[0] = c.red;
  p->color[1] = c.green;
  p->color[2] = c.blue;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _frame_colorpick_color_set(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_borders_params_t *p = self->params;

  // turn off the other color picker so that this tool actually works ...
  dt_iop_color_picker_reset(self, TRUE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  p->frame_color[0] = c.red;
  p->frame_color[1] = c.green;
  p->frame_color[2] = c.blue;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = self->gui_data;
  dt_iop_borders_params_t *p = self->params;

  gui_changed(self, NULL, NULL);

  // ----- Border Color
  GdkRGBA c = (GdkRGBA){.red   = p->color[0],
                        .green = p->color[1],
                        .blue  = p->color[2],
                        .alpha = 1.0 };

  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->colorpick), &c);

  // ----- Frame Color
  GdkRGBA fc = (GdkRGBA){
    .red   = p->frame_color[0],
    .green = p->frame_color[1],
    .blue  = p->frame_color[2],
    .alpha = 1.0
  };
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->frame_colorpick), &fc);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = IOP_GUI_ALLOC(borders);
  dt_iop_borders_params_t *p = self->params;
  dt_iop_borders_params_t *dp = self->default_params;

  g->basis = dt_bauhaus_combobox_from_params(self, "basis");
  gtk_widget_set_tooltip_text(g->basis,
                              _("which dimension to use for the size calculation"));

  g->size = dt_bauhaus_slider_from_params(self, "size");
  dt_bauhaus_slider_set_digits(g->size, 4);
  dt_bauhaus_slider_set_format(g->size, "%");
  gtk_widget_set_tooltip_text(g->size,
                              _("size of the border in percent of the chosen basis"));

  DT_BAUHAUS_COMBOBOX_NEW_FULL(g->aspect, self, NULL, N_("aspect"),
                               _("select the aspect ratio\n"
                                 "(right-click on slider below to type your own w:h)"),
                               0, _aspect_changed, self,
                               N_("image"),
                               N_("3:1"),
                               N_("95:33"),
                               N_("CinemaScope 2.39:1"),
                               N_("2:1"),
                               N_("16:9"),
                               N_("5:3"),
                               N_("US Legal 8.5x14"),
                               N_("golden cut"),
                               N_("16:10"),
                               N_("3:2 (4x6, 10x15cm)"),
                               N_("A4"),
                               N_("DIN"),
                               N_("7:5"),
                               N_("4:3"),
                               N_("US Letter 8.5x11"),
                               N_("14:11"),
                               N_("5:4 (8x10)"),
                               N_("square"),
                               N_("constant border"),
                               N_("custom..."));
  dt_gui_box_add(self->widget, g->aspect);

  g->aspect_slider = dt_bauhaus_slider_from_params(self, "aspect");
  gtk_widget_set_tooltip_text(g->aspect_slider, _("set the custom aspect ratio\n"
                                                  "(right-click to enter number or w:h)"));

  g->aspect_orient = dt_bauhaus_combobox_from_params(self, "aspect_orient");
  gtk_widget_set_tooltip_text(g->aspect_orient,
                              _("aspect ratio orientation of the image with border"));

  DT_BAUHAUS_COMBOBOX_NEW_FULL(g->pos_h, self, NULL, N_("horizontal position"),
                               _("select the horizontal position ratio relative to top\n"
                                 "(right-click on slider below to type your own x:w)"),
                               0, _position_h_changed, self,
                               N_("center"), N_("1/3"), N_("3/8"),
                               N_("5/8"), N_("2/3"), N_("custom..."));
  dt_gui_box_add(self->widget, g->pos_h);

  g->pos_h_slider = dt_bauhaus_slider_from_params(self, "pos_h");
  gtk_widget_set_tooltip_text(g->pos_h_slider, _("custom horizontal position"));

  DT_BAUHAUS_COMBOBOX_NEW_FULL(g->pos_v, self, NULL, N_("vertical position"),
                               _("select the vertical position ratio relative to left\n"
                                 "(right-click on slider below to type your own y:h)"),
                               0, _position_v_changed, self,
                               N_("center"), N_("1/3"), N_("3/8"),
                               N_("5/8"), N_("2/3"), N_("custom..."));
  dt_gui_box_add(self->widget, g->pos_v);

  g->pos_v_slider = dt_bauhaus_slider_from_params(self, "pos_v");
  gtk_widget_set_tooltip_text(g->pos_v_slider, _("custom vertical position"));

  g->frame_size = dt_bauhaus_slider_from_params(self, "frame_size");
  dt_bauhaus_slider_set_digits(g->frame_size, 4);
  dt_bauhaus_slider_set_format(g->frame_size, "%");
  gtk_widget_set_tooltip_text(g->frame_size,
                              _("size of the frame line in percent of min border width"));

  g->frame_offset = dt_bauhaus_slider_from_params(self, "frame_offset");
  dt_bauhaus_slider_set_digits(g->frame_offset, 4);
  dt_bauhaus_slider_set_format(g->frame_offset, "%");
  gtk_widget_set_tooltip_text(g->frame_offset,
                              _("offset of the frame line beginning on image side"));

  GdkRGBA color = (GdkRGBA){.red   = dp->color[0],
                            .green = dp->color[1],
                            .blue  = dp->color[2],
                            .alpha = 1.0 };

  GdkRGBA frame_color = (GdkRGBA){.red = dp->frame_color[0],
                                  .green = dp->frame_color[1],
                                  .blue = dp->frame_color[2],
                                  .alpha = 1.0 };

  GtkWidget *label, *box;

  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  label = dtgtk_reset_label_new(_("border color"), self, &p->color, 3 * sizeof(float));
  gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
  g->colorpick = gtk_color_button_new_with_rgba(&color);
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->colorpick), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->colorpick), _("select border color"));
  g_signal_connect(G_OBJECT(g->colorpick), "color-set",
                   G_CALLBACK(_colorpick_color_set), self);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->colorpick), FALSE, TRUE, 0);
  g->border_picker = dt_color_picker_new(self, DT_COLOR_PICKER_POINT, box);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->border_picker),
                              _("pick border color from image"));
  dt_action_define_iop(self, N_("pickers"), N_("border color"),
                       g->border_picker, &dt_action_def_toggle);
  dt_gui_box_add(self->widget, box);

  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  label = dtgtk_reset_label_new(_("frame line color"), self, &p->frame_color, 3 * sizeof(float));
  gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
  g->frame_colorpick = gtk_color_button_new_with_rgba(&frame_color);
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->frame_colorpick), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->frame_colorpick),
                             _("select frame line color"));
  g_signal_connect(G_OBJECT(g->frame_colorpick), "color-set",
                   G_CALLBACK(_frame_colorpick_color_set), self);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->frame_colorpick), FALSE, TRUE, 0);
  g->frame_picker = dt_color_picker_new(self, DT_COLOR_PICKER_POINT, box);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->frame_picker),
                              _("pick frame line color from image"));
  dt_action_define_iop(self, N_("pickers"), N_("frame line color"),
                       g->frame_picker, &dt_action_def_toggle);
  dt_gui_box_add(self->widget, box);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
