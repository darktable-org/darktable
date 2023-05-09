/*
    This file is part of darktable,
    Copyright (C) 2011-2023 darktable developers.

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

DT_MODULE_INTROSPECTION(3, dt_iop_borders_params_t)

// Module constants
#define DT_IOP_BORDERS_ASPECT_IMAGE_VALUE 0.0f
#define DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE -1.0f
typedef enum dt_iop_orientation_t
{
  DT_IOP_BORDERS_ASPECT_ORIENTATION_AUTO = 0,      // $DESCRIPTION: "auto"
  DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT = 1,  // $DESCRIPTION: "portrait"
  DT_IOP_BORDERS_ASPECT_ORIENTATION_LANDSCAPE = 2, // $DESCRIPTION: "landscape"
} dt_iop_orientation_t;

static const float _aspect_ratios[]
  = { DT_IOP_BORDERS_ASPECT_IMAGE_VALUE,
      3.0f, 95.0f / 33.0f, 2.39f, 2.0f, 16.0f / 9.0f, 5.0f / 3.0f, 14.0f / 8.5f, PHI, 3.0f / 2.0f,
      297.0f / 210.0f, M_SQRT2, 7.0f / 5.0f, 4.0f / 3.0f, 11.0f / 8.5f, 14.0f / 11.0f,
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
  float color[3];           // border color $DEFAULT: 1.0
  float aspect;             /* aspect ratio of the outer frame w/h
                               $MIN: 1.0 $MAX: 3.0 $DEFAULT: DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE $DESCRIPTION: "aspect ratio" */
  char aspect_text[20];     /* aspect ratio of the outer frame w/h (user string version)
                               DEFAULT: "constant border" */
  dt_iop_orientation_t aspect_orient;        /* aspect ratio orientation
                               $DEFAULT: 0 $DESCRIPTION: "orientation" */
  float size;               /* border width relative to overall frame width
                               $MIN: 0.0 $MAX: 0.5 $DEFAULT: 0.1 $DESCRIPTION: "border size" */
  float pos_h;              /* picture horizontal position ratio into the final image
                               $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "horizontal offset" */
  char pos_h_text[20];      /* picture horizontal position ratio into the final image (user string version)
                               DEFAULT: "1/2" */
  float pos_v;              /* picture vertical position ratio into the final image
                               $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "vertical offset"*/
  char pos_v_text[20];      /* picture vertical position ratio into the final image (user string version)
                               DEFAULT: "1/2" */
  float frame_size;         /* frame line width relative to border width
                               $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "frame line size" */
  float frame_offset;       /* frame offset from picture size relative to [border width - frame width]
                               $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "frame line offset" */
  float frame_color[3];     // frame line color $DEFAULT: 0.0
  gboolean max_border_size; /* the way border size is computed
                               $DEFAULT: TRUE */
} dt_iop_borders_params_t;

typedef struct dt_iop_borders_gui_data_t
{
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
int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 3)
  {
    typedef struct dt_iop_borders_params_v1_t
    {
      float color[3]; // border color
      float aspect;   // aspect ratio of the outer frame w/h
      float size;     // border width relative to overall frame width
    } dt_iop_borders_params_v1_t;

    dt_iop_borders_params_v1_t *o = (dt_iop_borders_params_v1_t *)old_params;
    dt_iop_borders_params_t *n = (dt_iop_borders_params_t *)new_params;
    dt_iop_borders_params_t *d = (dt_iop_borders_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters
    memcpy(n->color, o->color, sizeof(o->color));
    n->aspect = (o->aspect < 1) ? 1 / o->aspect : o->aspect;
    // no auto orientation in legacy param due to already convert aspect ratio
    n->aspect_orient = o->aspect > 1 ? DT_IOP_BORDERS_ASPECT_ORIENTATION_LANDSCAPE
                                     : DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT;
    n->size = fabsf(o->size); // no negative size any more (was for "constant border" detect)
    n->max_border_size = FALSE;
    return 0;
  }

  if(old_version == 2 && new_version == 3)
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

    dt_iop_borders_params_v2_t *o = (dt_iop_borders_params_v2_t *)old_params;
    dt_iop_borders_params_t *n = (dt_iop_borders_params_t *)new_params;

    memcpy(n, o, sizeof(struct dt_iop_borders_params_v2_t));
    n->max_border_size = FALSE;
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

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("add solid borders or margins around the picture"),
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
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_GUIDES_WIDGET;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *const restrict points, size_t points_count)
{
  dt_iop_borders_data_t *d = (dt_iop_borders_data_t *)piece->data;

  const int border_tot_width = (piece->buf_out.width - piece->buf_in.width);
  const int border_tot_height = (piece->buf_out.height - piece->buf_in.height);
  const int border_size_t = border_tot_height * d->pos_v;
  const int border_size_l = border_tot_width * d->pos_h;

  // nothing to be done if parameters are set to neutral values (no top/left border)
  if(border_size_l == 0 && border_size_t == 0) return 1;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(points, points_count, border_size_l, border_size_t)  \
  schedule(static) if(points_count > 100) aligned(points:64)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    points[i] += border_size_l;
    points[i + 1] += border_size_t;
  }

  return 1;
}
int distort_backtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *const restrict points,
                          size_t points_count)
{
  dt_iop_borders_data_t *d = (dt_iop_borders_data_t *)piece->data;

  const int border_tot_width = (piece->buf_out.width - piece->buf_in.width);
  const int border_tot_height = (piece->buf_out.height - piece->buf_in.height);
  const int border_size_t = border_tot_height * d->pos_v;
  const int border_size_l = border_tot_width * d->pos_h;

  // nothing to be done if parameters are set to neutral values (no top/left border)
  if(border_size_l == 0 && border_size_t == 0) return 1;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(points, points_count, border_size_l, border_size_t)  \
  schedule(static) if(points_count > 100) aligned(points:64)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    points[i] -= border_size_l;
    points[i + 1] -= border_size_t;
  }

  return 1;
}

void distort_mask(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const float *const in,
                  float *const out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_borders_data_t *d = (dt_iop_borders_data_t *)piece->data;

  const int border_tot_width = (piece->buf_out.width - piece->buf_in.width) * roi_in->scale;
  const int border_tot_height = (piece->buf_out.height - piece->buf_in.height) * roi_in->scale;
  const int border_size_t = border_tot_height * d->pos_v;
  const int border_size_l = border_tot_width * d->pos_h;
  const int border_in_x = MAX(border_size_l - roi_out->x, 0);
  const int border_in_y = MAX(border_size_t - roi_out->y, 0);

  // fill the image with 0 so that the added border isn't part of the mask
  dt_iop_image_fill(out, 0.0f, roi_out->width, roi_out->height, 1);

  // blit image inside border and fill the output with previous processed out
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(roi_in, roi_out, border_in_x, border_in_y, in, out)   \
  schedule(static)
#endif
  for(int j = 0; j < roi_in->height; j++)
  {
    float *outb = out + (size_t)(j + border_in_y) * roi_out->width + border_in_x;
    const float *inb = in + (size_t)j * roi_in->width;
    memcpy(outb, inb, sizeof(float) * roi_in->width);
  }
}

// 1st pass: how large would the output be, given this input roi?
// this is always called with the full buffer before processing.
void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
  dt_iop_borders_data_t *d = (dt_iop_borders_data_t *)piece->data;

  const float size = fabsf(d->size);
  if(size == 0) return;

  if(d->aspect == DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE)
  {
    // for a constant border be sure to base the computation on the
    // larger border, failing that the border will have a difference
    // size depending on the orientation.

    if(roi_in->width > roi_in->height || !d->max_border_size)
    {
      // this means: relative to width and constant for height as well:
      roi_out->width = roundf((float)roi_in->width / (1.0f - size));
      roi_out->height = roi_in->height + roi_out->width - roi_in->width;
    }
    else
    {
      // this means: relative to height and constant for width as well:
      roi_out->height = roundf((float)roi_in->height / (1.0f - size));
      roi_out->width = roi_in->width + roi_out->height - roi_in->height;
    }
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

    // min width: constant ratio based on size:
    roi_out->width = roundf((float)roi_in->width / (1.0f - size));
    // corresponding height: determined by aspect ratio:
    roi_out->height = roundf((float)roi_out->width / aspect);
    // insane settings used?
    if(roi_out->height < (float)roi_in->height / (1.0f - size))
    {
      roi_out->height = roundf((float)roi_in->height / (1.0f - size));
      roi_out->width = roundf((float)roi_out->height * aspect);
    }
  }

  // sanity check.
  const size_t max_dim = MAX(roi_in->width, roi_in->height);
  roi_out->width = CLAMP(roi_out->width, 1, 3 * max_dim);
  roi_out->height = CLAMP(roi_out->height, 1, 3 * max_dim);
}

// 2nd pass: which roi would this operation need as input to fill the given output region?
void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  dt_iop_borders_data_t *d = (dt_iop_borders_data_t *)piece->data;
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

struct border_positions_t
{
  dt_aligned_pixel_t bcolor;
  dt_aligned_pixel_t flcolor;
  int border_top;		// 0..bt is rows of top border outside the frameline
  int fl_top;			//bt..ft is the top frameline
  int image_top;		//ft..it is the top border inside the frameline
  int border_left;		// 0..bl is columns of left border outside the frameline
  int fl_left;			//bl..fl is the left frameline
  int image_left;		//fl..il is the left border inside the frameline
  int image_right;		//il..ir is the actual image area
  int fl_right;			//ir..fr is the right border inside the frameline
  int border_right;		//fr..br is the right frameeline
  int width;			//br..width is the right border outside the frameline
  int image_bot;		//it..ib is the actual image area
  int fl_bot;			//ib..fb is the bottom border inside the frameline
  int border_bot;		//fb..bt is the frameline
  int height;			//bt..height is the bottom border outside the frameline
  int stride;			// width of input roi
};

// this will be called from inside an OpenMP parallel section, so no need to parallelize further
static inline void set_pixels(float *buf, const dt_aligned_pixel_t color, const int npixels)
{
  for(int i = 0; i < npixels; i++)
  {
    copy_pixel_nontemporal(buf + 4*i,  color);
  }
}

// this will be called from inside an OpenMP parallel section, so no need to parallelize further
static inline void copy_pixels(float *out, const float *const in, const int npixels)
{
  for(int i = 0; i < npixels; i++)
  {
    copy_pixel_nontemporal(out + 4*i, in + 4*i);
  }
}

void copy_image_with_border(float *out, const float *const in, const struct border_positions_t *binfo)
{
  const int image_width = binfo->image_right - binfo->image_left;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, out, binfo, image_width) \
  schedule(static)
#endif
  for(size_t row = 0; row < binfo->height; row++)
  {
    float *outrow = out + 4 * row * binfo->width;
    if(row < binfo->border_top || row >= binfo->border_bot)
    {
      // top/bottom border outside the frameline: entirely the border color
      set_pixels(outrow, binfo->bcolor, binfo->width);
    }
    else if(row < binfo->fl_top || row >= binfo->fl_bot)
    {
      // top/bottom frameline
      set_pixels(outrow, binfo->bcolor, binfo->border_left);
      set_pixels(outrow + 4*binfo->border_left, binfo->flcolor, binfo->border_right - binfo->border_left);
      set_pixels(outrow + 4*binfo->border_right, binfo->bcolor, binfo->width - binfo->border_right);
    }
    else if(row < binfo->image_top || row >= binfo->image_bot)
    {
      // top/bottom border inside the frameline
      set_pixels(outrow, binfo->bcolor, binfo->border_left);
      set_pixels(outrow + 4*binfo->border_left, binfo->flcolor, binfo->fl_left - binfo->border_left);
      set_pixels(outrow + 4*binfo->fl_left, binfo->bcolor, binfo->fl_right - binfo->fl_left);
      set_pixels(outrow + 4*binfo->fl_right, binfo->flcolor, binfo->border_right - binfo->fl_right);
      set_pixels(outrow + 4*binfo->border_right, binfo->bcolor, binfo->width - binfo->border_right);
    }
    else
    {
      // image area: set left border (w/optional frame line), copy image row, set right border (w/optional frame line)
      // set outer border
      set_pixels(outrow, binfo->bcolor, binfo->border_left);
      if(binfo->image_left > binfo->border_left)
      {
        // we have a frameline, so set it and the inner border
        set_pixels(outrow + 4*binfo->border_left, binfo->flcolor, binfo->fl_left - binfo->border_left);
        set_pixels(outrow + 4*binfo->fl_left, binfo->bcolor, binfo->image_left - binfo->fl_left);
      }
      // copy image row
      copy_pixels(outrow + 4*binfo->image_left, in + 4 * (row - binfo->image_top) * binfo->stride, image_width);
      // set right border
      set_pixels(outrow + 4*binfo->image_right, binfo->bcolor, binfo->fl_right - binfo->image_right);
      if(binfo->width > binfo->fl_right)
      {
        // we have a frameline, so set it and the outer border
        set_pixels(outrow + 4*binfo->fl_right, binfo->flcolor, binfo->border_right - binfo->fl_right);
        set_pixels(outrow + 4*binfo->border_right, binfo->bcolor, binfo->width - binfo->border_right);
      }
    }
  }
  // ensure that all streaming writes complete before we attempt to read from the output buffer
  dt_omploop_sfence();
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_borders_data_t *const d = (dt_iop_borders_data_t *)piece->data;

  const int border_tot_width = (piece->buf_out.width - piece->buf_in.width) * roi_in->scale;
  const int border_tot_height = (piece->buf_out.height - piece->buf_in.height) * roi_in->scale;
  const int border_size_t = border_tot_height * d->pos_v;
  const int border_size_b = border_tot_height - border_size_t;
  const int border_size_l = border_tot_width * d->pos_h;
  const int border_size_r = border_tot_width - border_size_l;
  const int border_in_x = MAX(border_size_l - roi_out->x, 0);
  const int border_in_y = MAX(border_size_t - roi_out->y, 0);

  // compute frame line parameters
  const int border_min_size = MIN(MIN(border_size_t, border_size_b), MIN(border_size_l, border_size_r));
  const int frame_size = border_min_size * d->frame_size;

  const int b_in_x = CLAMP(border_in_x, 0, roi_out->width - 1);
  struct border_positions_t binfo =
    { .bcolor = { d->color[0], d->color[1], d->color[2], 1.0f },
      .flcolor = { d->frame_color[0], d->frame_color[1], d->frame_color[2], 1.0f },
      .border_top = border_in_y,
      .fl_top = border_in_y,
      .image_top = border_in_y,
      .border_left = b_in_x,
      .fl_left = b_in_x,
      .image_left = b_in_x,
      .image_right = b_in_x + roi_in->width,
      .fl_right = roi_out->width,
      .border_right = roi_out->width,
      .width = roi_out->width,
      .image_bot = border_in_y + roi_in->height,
      .fl_bot = roi_out->height,
      .border_bot = roi_out->height,
      .height = roi_out->height,
      .stride = roi_in->width
  };
  if(frame_size > 0)
  {
    const int image_lx = border_size_l - roi_out->x;
    const int image_ty = border_size_t - roi_out->y;
    const int frame_space = border_min_size - frame_size;
    const int frame_offset = frame_space * d->frame_offset;
    const int frame_tl_in_x = MAX(border_in_x - frame_offset, 0);
    const int frame_tl_out_x = MAX(frame_tl_in_x - frame_size, 0);
    const int frame_tl_in_y = MAX(border_in_y - frame_offset, 0);
    const int frame_tl_out_y = MAX(frame_tl_in_y - frame_size, 0);
    binfo.border_top = frame_tl_out_y;
    binfo.fl_top = frame_tl_in_y;
    binfo.border_left = CLAMP(frame_tl_out_x, 0, roi_out->width);
    binfo.fl_left = CLAMP(frame_tl_in_x, 0, roi_out->width);
    const int frame_in_width = floor((piece->buf_in.width * roi_in->scale) + frame_offset * 2);
    const int frame_in_height = floor((piece->buf_in.height * roi_in->scale) + frame_offset * 2);
    const int frame_out_width = frame_in_width + frame_size * 2;
    const int frame_out_height = frame_in_height + frame_size * 2;
    const int frame_br_in_x = CLAMP(image_lx - frame_offset + frame_in_width - 1, 0, roi_out->width - 1);
    const int frame_br_in_y = CLAMP(image_ty - frame_offset + frame_in_height - 1, 0, roi_out->height - 1);
    // ... if 100% frame_offset we ensure frame_line "stick" the out border
    const int frame_br_out_x
        = (d->frame_offset == 1.0f && (border_min_size == MIN(border_size_l, border_size_r)))
              ? (roi_out->width)
              : CLAMP(image_lx - frame_offset - frame_size + frame_out_width - 1, 0, roi_out->width - 1);
    const int frame_br_out_y
        = (d->frame_offset == 1.0f && (border_min_size == MIN(border_size_t, border_size_b)))
              ? (roi_out->height)
              : CLAMP(image_ty - frame_offset - frame_size + frame_out_height - 1, 0, roi_out->height - 1);
    binfo.fl_right = frame_br_in_x + 1;		// need end+1 for these coordinates
    binfo.border_right = frame_br_out_x + 1;
    binfo.fl_bot = frame_br_in_y + 1;
    binfo.border_bot = frame_br_out_y + 1;
  }
  copy_image_with_border((float*)ovoid, (const float*)ivoid, &binfo);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_borders_data_t *d = (dt_iop_borders_data_t *)piece->data;
  dt_iop_borders_global_data_t *gd = (dt_iop_borders_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;

  const int width = roi_out->width;
  const int height = roi_out->height;
  size_t sizes[2] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid) };

  const int border_tot_width = (piece->buf_out.width - piece->buf_in.width) * roi_in->scale;
  const int border_tot_height = (piece->buf_out.height - piece->buf_in.height) * roi_in->scale;
  const int border_size_t = border_tot_height * d->pos_v;
  const int border_size_b = border_tot_height - border_size_t;
  const int border_size_l = border_tot_width * d->pos_h;
  const int border_size_r = border_tot_width - border_size_l;
  const int border_in_x = MAX(border_size_l - roi_out->x, 0);
  const int border_in_y = MAX(border_size_t - roi_out->y, 0);

  // ----- Filling border
  const float col[4] = { d->color[0], d->color[1], d->color[2], 1.0f };
  const int zero = 0;
  dt_opencl_set_kernel_args(devid, gd->kernel_borders_fill, 0, CLARG(dev_out), CLARG(zero), CLARG(zero),
    CLARG(width), CLARG(height), CLARG(col));
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_borders_fill, sizes);
  if(err != CL_SUCCESS) goto error;

  // ----- Frame line
  const int border_min_size = MIN(MIN(border_size_t, border_size_b), MIN(border_size_l, border_size_r));
  const int frame_size = border_min_size * d->frame_size;
  if(frame_size != 0)
  {
    const float col_frame[4] = { d->frame_color[0], d->frame_color[1], d->frame_color[2], 1.0f };
    const int image_lx = border_size_l - roi_out->x;
    const int image_ty = border_size_t - roi_out->y;
    const int frame_space = border_min_size - frame_size;
    const int frame_offset = frame_space * d->frame_offset;
    const int frame_tl_in_x = MAX(border_in_x - frame_offset, 0);
    const int frame_tl_out_x = MAX(frame_tl_in_x - frame_size, 0);
    const int frame_tl_in_y = MAX(border_in_y - frame_offset, 0);
    const int frame_tl_out_y = MAX(frame_tl_in_y - frame_size, 0);
    const int frame_in_width = floor((piece->buf_in.width * roi_in->scale) + frame_offset * 2);
    const int frame_in_height = floor((piece->buf_in.height * roi_in->scale) + frame_offset * 2);
    const int frame_out_width = frame_in_width + frame_size * 2;
    const int frame_out_height = frame_in_height + frame_size * 2;
    const int frame_br_in_x = CLAMP(image_lx - frame_offset + frame_in_width - 1, 0, roi_out->width - 1);
    const int frame_br_in_y = CLAMP(image_ty - frame_offset + frame_in_height - 1, 0, roi_out->height - 1);
    // ... if 100% frame_offset we ensure frame_line "stick" the out border
    const int frame_br_out_x
        = (d->frame_offset == 1.0f && (border_min_size == MIN(border_size_l, border_size_r)))
              ? (roi_out->width)
              : CLAMP(image_lx - frame_offset - frame_size + frame_out_width - 1, 0, roi_out->width);
    const int frame_br_out_y
        = (d->frame_offset == 1.0f && (border_min_size == MIN(border_size_t, border_size_b)))
              ? (roi_out->height)
              : CLAMP(image_ty - frame_offset - frame_size + frame_out_height - 1, 0, roi_out->height);

    const int roi_frame_in_width = frame_br_in_x - frame_tl_in_x;
    const int roi_frame_in_height = frame_br_in_y - frame_tl_in_y;
    const int roi_frame_out_width = frame_br_out_x - frame_tl_out_x;
    const int roi_frame_out_height = frame_br_out_y - frame_tl_out_y;

    dt_opencl_set_kernel_args(devid, gd->kernel_borders_fill, 0, CLARG(dev_out), CLARG(frame_tl_out_x),
      CLARG(frame_tl_out_y), CLARG(roi_frame_out_width), CLARG(roi_frame_out_height), CLARG(col_frame));
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_borders_fill, sizes);
    if(err != CL_SUCCESS) goto error;

    dt_opencl_set_kernel_args(devid, gd->kernel_borders_fill, 0, CLARG(dev_out), CLARG(frame_tl_in_x),
      CLARG(frame_tl_in_y), CLARG(roi_frame_in_width), CLARG(roi_frame_in_height), CLARG(col));
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_borders_fill, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  size_t iorigin[] = { 0, 0, 0 };
  size_t oorigin[] = { border_in_x, border_in_y, 0 };
  size_t region[] = { roi_in->width, roi_in->height, 1 };

  // copy original input from dev_in -> dev_out as starting point
  err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, iorigin, oorigin, region);
  if(err != CL_SUCCESS) goto error;

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_borders] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif


void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl from programs.conf
  dt_iop_borders_global_data_t *gd
      = (dt_iop_borders_global_data_t *)malloc(sizeof(dt_iop_borders_global_data_t));
  module->data = gd;
  gd->kernel_borders_fill = dt_opencl_create_kernel(program, "borders_fill");
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_borders_global_data_t *gd = (dt_iop_borders_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_borders_fill);
  free(module->data);
  module->data = NULL;
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)p1;
  dt_iop_borders_data_t *d = (dt_iop_borders_data_t *)piece->data;
  memcpy(d, p, sizeof(dt_iop_borders_params_t));
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_borders_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
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
                                                         TRUE };
  dt_gui_presets_add_generic(_("15:10 postcard white"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_NONE);

  p.color[0] = p.color[1] = p.color[2] = 0.0f;
  p.frame_color[0] = p.frame_color[1] = p.frame_color[2] = 1.0f;
  dt_gui_presets_add_generic(_("15:10 postcard black"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_NONE);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;

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

static void aspect_changed(GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;
  const int which = dt_bauhaus_combobox_get(combo);
  const char *text = dt_bauhaus_combobox_get_text(combo);
  if(which == dt_bauhaus_combobox_length(combo)-1)
  {
    g_strlcpy(p->aspect_text, text, sizeof(p->aspect_text));
  }
  else if(which < DT_IOP_BORDERS_ASPECT_COUNT)
  {
    g_strlcpy(p->aspect_text, text, sizeof(p->aspect_text));
    p->aspect = _aspect_ratios[which];
    ++darktable.gui->reset;
    dt_bauhaus_slider_set(g->aspect_slider,p->aspect);
    --darktable.gui->reset;
  }
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void position_h_changed(GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;
  const int which = dt_bauhaus_combobox_get(combo);
  const char *text = dt_bauhaus_combobox_get_text(combo);
  if(which == dt_bauhaus_combobox_length(combo)-1)
  {
    g_strlcpy(p->aspect_text, text, sizeof(p->aspect_text));
  }
  else if(which < DT_IOP_BORDERS_POSITION_H_COUNT)
  {
    g_strlcpy(p->pos_h_text, text, sizeof(p->pos_h_text));
    p->pos_h = _pos_h_ratios[which];
    ++darktable.gui->reset;
    dt_bauhaus_slider_set(g->pos_h_slider,p->pos_h);
    --darktable.gui->reset;
  }
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void position_v_changed(GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;
  const int which = dt_bauhaus_combobox_get(combo);
  const char *text = dt_bauhaus_combobox_get_text(combo);
  if(which == dt_bauhaus_combobox_length(combo)-1)
  {
    g_strlcpy(p->aspect_text, text, sizeof(p->aspect_text));
  }
  else if(which < DT_IOP_BORDERS_POSITION_V_COUNT)
  {
    g_strlcpy(p->pos_v_text, text, sizeof(p->pos_v_text));
    p->pos_v = _pos_v_ratios[which];
    ++darktable.gui->reset;
    dt_bauhaus_slider_set(g->pos_v_slider,p->pos_v);
    --darktable.gui->reset;
  }
  dt_iop_color_picker_reset(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;

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
  else if(!w || w == g->pos_h_slider)
  {
    for(k = 0; k < DT_IOP_BORDERS_POSITION_H_COUNT; k++)
    {
      if(fabsf(p->pos_h - _pos_h_ratios[k]) < 0.01f)
        break;
    }
    dt_bauhaus_combobox_set(g->pos_h, k);
  }
  else if(!w || w == g->pos_v_slider)
  {
    for(k = 0; k < DT_IOP_BORDERS_POSITION_V_COUNT; k++)
    {
      if(fabsf(p->pos_v - _pos_v_ratios[k]) < 0.01f)
        break;
    }
    dt_bauhaus_combobox_set(g->pos_v, k);
  }
}

static void colorpick_color_set(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;

  // turn off the other color picker so that this tool actually works ...
  dt_iop_color_picker_reset(self, TRUE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  p->color[0] = c.red;
  p->color[1] = c.green;
  p->color[2] = c.blue;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void frame_colorpick_color_set(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;

  // turn off the other color picker so that this tool actually works ...
  dt_iop_color_picker_reset(self, TRUE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  p->frame_color[0] = c.red;
  p->frame_color[1] = c.green;
  p->frame_color[2] = c.blue;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;

  gui_changed(self, NULL, NULL);

  // ----- Border Color
  GdkRGBA c = (GdkRGBA){.red = p->color[0], .green = p->color[1], .blue = p->color[2], .alpha = 1.0 };
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->colorpick), &c);

  // ----- Frame Color
  GdkRGBA fc = (GdkRGBA){
    .red = p->frame_color[0], .green = p->frame_color[1], .blue = p->frame_color[2], .alpha = 1.0
  };
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->frame_colorpick), &fc);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = IOP_GUI_ALLOC(borders);
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->default_params;

  g->size = dt_bauhaus_slider_from_params(self, "size");
  dt_bauhaus_slider_set_digits(g->size, 4);
  dt_bauhaus_slider_set_format(g->size, "%");
  gtk_widget_set_tooltip_text(g->size, _("size of the border in percent of the full image"));

  DT_BAUHAUS_COMBOBOX_NEW_FULL(g->aspect, self, NULL, N_("aspect"),
                               _("select the aspect ratio (right click on slider below to type your own w:h)"),
                               0, aspect_changed, self,
                               N_("image"),
                               N_("3:1"),
                               N_("95:33"),
                               N_("Cinemascope 2.39:1"),
                               N_("2:1"),
                               N_("16:9"),
                               N_("5:3"),
                               N_("US Legal 8.5x14"),
                               N_("golden cut"),
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
  dt_bauhaus_combobox_set_editable(g->aspect, 1);
  gtk_box_pack_start(GTK_BOX(self->widget), g->aspect, TRUE, TRUE, 0);

  g->aspect_slider = dt_bauhaus_slider_from_params(self, "aspect");
  gtk_widget_set_tooltip_text(g->aspect_slider, _("set the custom aspect ratio (right click to enter number or w:h)"));

  g->aspect_orient = dt_bauhaus_combobox_from_params(self, "aspect_orient");
  gtk_widget_set_tooltip_text(g->aspect_orient, _("aspect ratio orientation of the image with border"));

  DT_BAUHAUS_COMBOBOX_NEW_FULL(g->pos_h, self, NULL, N_("horizontal position"),
                               _("select the horizontal position ratio relative to top "
                                 "or right click and type your own (y:h)"),
                               0, position_h_changed, self,
                               N_("center"), N_("1/3"), N_("3/8"), N_("5/8"), N_("2/3"), N_("custom..."));
  dt_bauhaus_combobox_set_editable(g->pos_h, 1);
  gtk_box_pack_start(GTK_BOX(self->widget), g->pos_h, TRUE, TRUE, 0);

  g->pos_h_slider = dt_bauhaus_slider_from_params(self, "pos_h");
  gtk_widget_set_tooltip_text(g->pos_h_slider, _("custom horizontal position"));

  DT_BAUHAUS_COMBOBOX_NEW_FULL(g->pos_v, self, NULL, N_("vertical position"),
                               _("select the vertical position ratio relative to left "
                                 "or right click and type your own (x:w)"),
                               0, position_v_changed, self,
                               N_("center"), N_("1/3"), N_("3/8"), N_("5/8"), N_("2/3"), N_("custom..."));
  dt_bauhaus_combobox_set_editable(g->pos_v, 1);
  gtk_box_pack_start(GTK_BOX(self->widget), g->pos_v, TRUE, TRUE, 0);

  g->pos_v_slider = dt_bauhaus_slider_from_params(self, "pos_v");
  gtk_widget_set_tooltip_text(g->pos_v_slider, _("custom vertical position"));

  g->frame_size = dt_bauhaus_slider_from_params(self, "frame_size");
  dt_bauhaus_slider_set_digits(g->frame_size, 4);
  dt_bauhaus_slider_set_format(g->frame_size, "%");
  gtk_widget_set_tooltip_text(g->frame_size, _("size of the frame line in percent of min border width"));

  g->frame_offset = dt_bauhaus_slider_from_params(self, "frame_offset");
  dt_bauhaus_slider_set_digits(g->frame_offset, 4);
  dt_bauhaus_slider_set_format(g->frame_offset, "%");
  gtk_widget_set_tooltip_text(g->frame_offset, _("offset of the frame line beginning on picture side"));

  GdkRGBA color = (GdkRGBA){.red = p->color[0], .green = p->color[1], .blue = p->color[2], .alpha = 1.0 };

  GtkWidget *label, *box;

  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  label = dtgtk_reset_label_new(_("border color"), self, &p->color, 3 * sizeof(float));
  gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
  g->colorpick = gtk_color_button_new_with_rgba(&color);
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->colorpick), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->colorpick), _("select border color"));
  g_signal_connect(G_OBJECT(g->colorpick), "color-set", G_CALLBACK(colorpick_color_set), self);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->colorpick), FALSE, TRUE, 0);
  g->border_picker = dt_color_picker_new(self, DT_COLOR_PICKER_POINT, box);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->border_picker), _("pick border color from image"));
  dt_action_define_iop(self, N_("pickers"), N_("border color"), g->border_picker, &dt_action_def_toggle);
  gtk_box_pack_start(GTK_BOX(self->widget), box, TRUE, TRUE, 0);

  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  label = dtgtk_reset_label_new(_("frame line color"), self, &p->color, 3 * sizeof(float));
  gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
  g->frame_colorpick = gtk_color_button_new_with_rgba(&color);
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->frame_colorpick), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->frame_colorpick), _("select frame line color"));
  g_signal_connect(G_OBJECT(g->frame_colorpick), "color-set", G_CALLBACK(frame_colorpick_color_set), self);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->frame_colorpick), FALSE, TRUE, 0);
  g->frame_picker = dt_color_picker_new(self, DT_COLOR_PICKER_POINT, box);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->frame_picker), _("pick frame line color from image"));
  dt_action_define_iop(self, N_("pickers"), N_("frame line color"), g->frame_picker, &dt_action_def_toggle);
  gtk_box_pack_start(GTK_BOX(self->widget), box, TRUE, TRUE, 0);
}


void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);

  dt_iop_borders_params_t *defaults = self->default_params;

  g_strlcpy(defaults->aspect_text, "constant border", sizeof(defaults->aspect_text));
  g_strlcpy(defaults->pos_h_text, "1/2", sizeof(defaults->pos_h_text));
  g_strlcpy(defaults->pos_v_text, "1/2", sizeof(defaults->pos_v_text));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
