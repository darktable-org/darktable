/*
    This file is part of darktable,
    copyright (c) 2011 johannes hanika.

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
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/debug.h"
#include "common/opencl.h"
#include "bauhaus/bauhaus.h"
#include "dtgtk/resetlabel.h"
#include "dtgtk/togglebutton.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "gui/presets.h"

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <gdk/gdkkeysyms.h>

DT_MODULE_INTROSPECTION(3, dt_iop_borders_params_t)

// Module constants
#define DT_IOP_BORDERS_ASPECT_COUNT 21
#define DT_IOP_BORDERS_ASPECT_IMAGE_IDX 0
#define DT_IOP_BORDERS_ASPECT_CONSTANT_IDX 11
#define DT_IOP_BORDERS_ASPECT_IMAGE_VALUE 0.0f
#define DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE -1.0f
#define DT_IOP_BORDERS_ASPECT_ORIENTATION_AUTO 0
#define DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT 1
#define DT_IOP_BORDERS_ASPECT_ORIENTATION_LANDSCAPE 2
#define DT_IOP_BORDERS_POSITION_H_COUNT 5
#define DT_IOP_BORDERS_POSITION_V_COUNT 5


typedef struct dt_iop_borders_params_t
{
  float color[3];           // border color
  float aspect;             // aspect ratio of the outer frame w/h
  char aspect_text[20];     // aspect ratio of the outer frame w/h (user string version)
  int aspect_orient;        // aspect ratio orientation
  float size;               // border width relative to overal frame width
  float pos_h;              // picture horizontal position ratio into the final image
  char pos_h_text[20];      // picture horizontal position ratio into the final image (user string version)
  float pos_v;              // picture vertical position ratio into the final image
  char pos_v_text[20];      // picture vertical position ratio into the final image (user string version)
  float frame_size;         // frame line width relative to border width
  float frame_offset;       // frame offset from picture size relative to [border width - frame width]
  float frame_color[3];     // frame line color
  gboolean max_border_size; // the way border size is computed
} dt_iop_borders_params_t;

typedef struct dt_iop_borders_gui_data_t
{
  GtkWidget *size;
  GtkWidget *aspect;
  GtkWidget *aspect_orient;
  GtkWidget *pos_h;
  GtkWidget *pos_v;
  GtkWidget *colorpick;
  GtkToggleButton *border_picker; // the 2nd button
  float aspect_ratios[DT_IOP_BORDERS_ASPECT_COUNT];
  float pos_h_ratios[DT_IOP_BORDERS_POSITION_H_COUNT];
  float pos_v_ratios[DT_IOP_BORDERS_POSITION_V_COUNT];
  GtkWidget *frame_size;
  GtkWidget *frame_offset;
  GtkWidget *frame_colorpick;
  GtkToggleButton *frame_picker; // the 2nd button
  GtkWidget *active_colorpick;
} dt_iop_borders_gui_data_t;

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 3)
  {
    typedef struct dt_iop_borders_params_v1_t
    {
      float color[3]; // border color
      float aspect;   // aspect ratio of the outer frame w/h
      float size;     // border width relative to overal frame width
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
      float size;           // border width relative to overal frame width
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

int groups()
{
  return IOP_GROUP_EFFECT;
}

int operation_tags()
{
  return IOP_TAG_DISTORT;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "border size"));
  dt_accel_register_iop(self, FALSE, NC_("accel", "pick border color from image"), 0, 0);
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "frame line size"));
  dt_accel_register_iop(self, FALSE, NC_("accel", "pick frame line color from image"), 0, 0);
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  dt_accel_connect_button_iop(self, "pick border color from image", GTK_WIDGET(g->colorpick));
  dt_accel_connect_slider_iop(self, "border size", GTK_WIDGET(g->size));
  dt_accel_connect_button_iop(self, "pick frame line color from image", GTK_WIDGET(g->frame_colorpick));
  dt_accel_connect_slider_iop(self, "frame line size", GTK_WIDGET(g->frame_size));
}

int distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points, size_t points_count)
{
  dt_iop_borders_data_t *d = (dt_iop_borders_data_t *)piece->data;

  const int border_tot_width = (piece->buf_out.width - piece->buf_in.width);
  const int border_tot_height = (piece->buf_out.height - piece->buf_in.height);
  const int border_size_t = border_tot_height * d->pos_v;
  const int border_size_l = border_tot_width * d->pos_h;

  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    points[i] += border_size_l;
    points[i + 1] += border_size_t;
  }

  return 1;
}
int distort_backtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points,
                          size_t points_count)
{
  dt_iop_borders_data_t *d = (dt_iop_borders_data_t *)piece->data;

  const int border_tot_width = (piece->buf_out.width - piece->buf_in.width);
  const int border_tot_height = (piece->buf_out.height - piece->buf_in.height);
  const int border_size_t = border_tot_height * d->pos_v;
  const int border_size_l = border_tot_width * d->pos_h;

  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    points[i] -= border_size_l;
    points[i + 1] -= border_size_t;
  }

  return 1;
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
    // for a constant border be sure to base the computation on the larger border, failing that the border
    // will have a difference size depending on the orientation.

    if(roi_in->width > roi_in->height || !d->max_border_size)
    {
      // this means: relative to width and constant for height as well:
      roi_out->width = (float)roi_in->width / (1.0f - size);
      roi_out->height = roi_in->height + roi_out->width - roi_in->width;
    }
    else
    {
      // this means: relative to height and constant for width as well:
      roi_out->height = (float)roi_in->height / (1.0f - size);
      roi_out->width = roi_in->width + roi_out->height - roi_in->height;
    }
  }
  else
  {
    float image_aspect = roi_in->width / (float)(roi_in->height);
    float aspect = (d->aspect == DT_IOP_BORDERS_ASPECT_IMAGE_VALUE) ? image_aspect : d->aspect;

    if(d->aspect_orient == DT_IOP_BORDERS_ASPECT_ORIENTATION_AUTO)
      aspect = ((image_aspect < 1 && aspect > 1) || (image_aspect > 1 && aspect < 1)) ? 1 / aspect : aspect;
    else if(d->aspect_orient == DT_IOP_BORDERS_ASPECT_ORIENTATION_LANDSCAPE)
      aspect = (aspect < 1) ? 1 / aspect : aspect;
    else if(d->aspect_orient == DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT)
      aspect = (aspect > 1) ? 1 / aspect : aspect;

    // min width: constant ratio based on size:
    roi_out->width = (float)roi_in->width / (1.0f - size);
    // corresponding height: determined by aspect ratio:
    roi_out->height = (float)roi_out->width / aspect;
    // insane settings used?
    if(roi_out->height < (float)roi_in->height / (1.0f - size))
    {
      roi_out->height = (float)roi_in->height / (1.0f - size);
      roi_out->width = (float)roi_out->height * aspect;
    }
  }

  // sanity check.
  roi_out->width = CLAMP(roi_out->width, 1, 3 * roi_in->width);
  roi_out->height = CLAMP(roi_out->height, 1, 3 * roi_in->height);
}

// 2nd pass: which roi would this operation need as input to fill the given output region?
void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  dt_iop_borders_data_t *d = (dt_iop_borders_data_t *)piece->data;
  *roi_in = *roi_out;
  const int bw = (piece->buf_out.width - piece->buf_in.width) * roi_out->scale;
  const int bh = (piece->buf_out.height - piece->buf_in.height) * roi_out->scale;

  // don't request outside image (no px for borders)
  roi_in->x = MAX(roi_out->x - bw * d->pos_h, 0);
  roi_in->y = MAX(roi_out->y - bh * d->pos_v, 0);
  // subtract upper left border from dimensions
  roi_in->width -= MAX(bw * d->pos_h - roi_out->x, 0);
  roi_in->height -= MAX(bh * d->pos_v - roi_out->y, 0);

  // subtract lower right border from dimensions
  roi_in->width -= roi_out->scale
                   * MAX((roi_in->x + roi_in->width) / roi_out->scale - (piece->buf_in.width), 0);
  roi_in->height -= roi_out->scale
                    * MAX((roi_in->y + roi_in->height) / roi_out->scale - (piece->buf_in.height), 0);
  // don't request nothing or outside roi
  roi_in->width = MIN(roi_out->scale * piece->buf_in.width, MAX(1, roi_in->width));
  roi_in->height = MIN(roi_out->scale * piece->buf_in.height, MAX(1, roi_in->height));
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_borders_data_t *d = (dt_iop_borders_data_t *)piece->data;

  const int ch = piece->colors;
  const size_t in_stride = (size_t)ch * roi_in->width;
  const size_t out_stride = (size_t)ch * roi_out->width;
  const size_t cp_stride = in_stride * sizeof(float);

  const int border_tot_width = (piece->buf_out.width - piece->buf_in.width) * roi_in->scale;
  const int border_tot_height = (piece->buf_out.height - piece->buf_in.height) * roi_in->scale;
  const int border_size_t = border_tot_height * d->pos_v;
  const int border_size_b = border_tot_height - border_size_t;
  const int border_size_l = border_tot_width * d->pos_h;
  const int border_size_r = border_tot_width - border_size_l;
  const int border_in_x = MAX(border_size_l - roi_out->x, 0);
  const int border_in_y = MAX(border_size_t - roi_out->y, 0);

  // Fill the out image with border color
  // sse-friendly color copy (stupidly copy whole buffer, /me lazy ass)
  const float col[4] = { d->color[0], d->color[1], d->color[2], 1.0f };
  float *buf = (float *)ovoid;
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++, buf += 4)
    memcpy(buf, col, sizeof(float) * 4);

  // Frame line draw
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
        = (d->frame_offset == 1.0f)
              ? (roi_out->width - 1)
              : CLAMP(image_lx - frame_offset - frame_size + frame_out_width - 1, 0, roi_out->width - 1);
    const int frame_br_out_y
        = (d->frame_offset == 1.0f)
              ? (roi_out->height - 1)
              : CLAMP(image_ty - frame_offset - frame_size + frame_out_height - 1, 0, roi_out->height - 1);

    for(int r = frame_tl_out_y; r <= frame_br_out_y; r++)
    {
      buf = (float *)ovoid + ((size_t)r * out_stride + frame_tl_out_x * ch);
      for(int c = frame_tl_out_x; c <= frame_br_out_x; c++, buf += 4)
        memcpy(buf, col_frame, sizeof(float) * 4);
    }
    for(int r = frame_tl_in_y; r <= frame_br_in_y; r++)
    {
      buf = (float *)ovoid + ((size_t)r * out_stride + frame_tl_in_x * ch);
      for(int c = frame_tl_in_x; c <= frame_br_in_x; c++, buf += 4) memcpy(buf, col, sizeof(float) * 4);
    }
  }

  // blit image inside border and fill the output with previous processed out
  for(int j = 0; j < roi_in->height; j++)
  {
    float *out = ((float *)ovoid) + (size_t)(j + border_in_y) * out_stride + ch * border_in_x;
    const float *in = ((float *)ivoid) + (size_t)j * in_stride;
    memcpy(out, in, cp_stride);
  }
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_borders_data_t *d = (dt_iop_borders_data_t *)piece->data;
  dt_iop_borders_global_data_t *gd = (dt_iop_borders_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;

  const int width = roi_out->width;
  const int height = roi_out->height;

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
  size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };
  const int zero = 0;
  dt_opencl_set_kernel_arg(devid, gd->kernel_borders_fill, 0, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_borders_fill, 1, sizeof(int), &zero);
  dt_opencl_set_kernel_arg(devid, gd->kernel_borders_fill, 2, sizeof(int), &zero);
  dt_opencl_set_kernel_arg(devid, gd->kernel_borders_fill, 3, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_borders_fill, 4, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_borders_fill, 5, 4 * sizeof(float), &col);
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
    const int frame_br_in_x = CLAMP(image_lx - frame_offset + frame_in_width, 0, roi_out->width);
    const int frame_br_in_y = CLAMP(image_ty - frame_offset + frame_in_height, 0, roi_out->height);
    // ... if 100% frame_offset we ensure frame_line "stick" the out border
    const int frame_br_out_x
        = (d->frame_offset == 1.0f)
              ? (roi_out->width)
              : CLAMP(image_lx - frame_offset - frame_size + frame_out_width, 0, roi_out->width);
    const int frame_br_out_y
        = (d->frame_offset == 1.0f)
              ? (roi_out->height)
              : CLAMP(image_ty - frame_offset - frame_size + frame_out_height, 0, roi_out->height);

    const int roi_frame_in_width = frame_br_in_x - frame_tl_in_x;
    const int roi_frame_in_height = frame_br_in_y - frame_tl_in_y;
    const int roi_frame_out_width = frame_br_out_x - frame_tl_out_x;
    const int roi_frame_out_height = frame_br_out_y - frame_tl_out_y;

    dt_opencl_set_kernel_arg(devid, gd->kernel_borders_fill, 0, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_borders_fill, 1, sizeof(int), &frame_tl_out_x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_borders_fill, 2, sizeof(int), &frame_tl_out_y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_borders_fill, 3, sizeof(int), &roi_frame_out_width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_borders_fill, 4, sizeof(int), &roi_frame_out_height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_borders_fill, 5, 4 * sizeof(float), &col_frame);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_borders_fill, sizes);
    if(err != CL_SUCCESS) goto error;

    dt_opencl_set_kernel_arg(devid, gd->kernel_borders_fill, 0, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_borders_fill, 1, sizeof(int), &frame_tl_in_x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_borders_fill, 2, sizeof(int), &frame_tl_in_y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_borders_fill, 3, sizeof(int), &roi_frame_in_width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_borders_fill, 4, sizeof(int), &roi_frame_in_height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_borders_fill, 5, 4 * sizeof(float), &col);
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
  dt_print(DT_DEBUG_OPENCL, "[opencl_borders] couldn't enqueue kernel! %d\n", err);
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
  self->commit_params(self, self->default_params, pipe, piece);
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
  dt_gui_presets_add_generic(_("15:10 postcard white"), self->op, self->version(), &p, sizeof(p), 1);
  p.color[0] = p.color[1] = p.color[2] = 0.0f;
  p.frame_color[0] = p.frame_color[1] = p.frame_color[2] = 1.0f;
  dt_gui_presets_add_generic(_("15:10 postcard black"), self->op, self->version(), &p, sizeof(p), 1);
}

static void request_pick_toggled(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  self->request_color_pick
      = (gtk_toggle_button_get_active(togglebutton) ? DT_REQUEST_COLORPICK_MODULE : DT_REQUEST_COLORPICK_OFF);

  /* use point sample */
  if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF)
  {
    dt_lib_colorpicker_set_point(darktable.lib, 0.5, 0.5);
    dt_dev_reprocess_all(self->dev);
  }
  else
    dt_control_queue_redraw();

  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  dt_iop_request_focus(self);
}

static void request_pick_toggled_border(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  g->active_colorpick = g->colorpick;
  request_pick_toggled(togglebutton, self);
}

static void request_pick_toggled_frame(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  g->active_colorpick = g->frame_colorpick;
  request_pick_toggled(togglebutton, self);
}

static gboolean borders_draw(GtkWidget *widget, cairo_t *cr, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;
  if(self->picked_output_color_max[0] < 0) return FALSE;
  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF) return FALSE;
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;

  if(fabsf(p->color[0] - self->picked_output_color[0]) < 0.0001f
     && fabsf(p->color[1] - self->picked_output_color[1]) < 0.0001f
     && fabsf(p->color[2] - self->picked_output_color[2]) < 0.0001f)
  {
    // interrupt infinite loops
    return FALSE;
  }

  if(fabsf(p->frame_color[0] - self->picked_output_color[0]) < 0.0001f
     && fabsf(p->frame_color[1] - self->picked_output_color[1]) < 0.0001f
     && fabsf(p->frame_color[2] - self->picked_output_color[2]) < 0.0001f)
  {
    // interrupt infinite loops
    return FALSE;
  }

  GdkRGBA c = (GdkRGBA){.red = self->picked_output_color[0],
                        .green = self->picked_output_color[1],
                        .blue = self->picked_output_color[2],
                        .alpha = 1.0 };
  if(g->active_colorpick == g->frame_colorpick)
  {
    p->frame_color[0] = self->picked_output_color[0];
    p->frame_color[1] = self->picked_output_color[1];
    p->frame_color[2] = self->picked_output_color[2];
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->frame_colorpick), &c);
  }
  else
  {
    p->color[0] = self->picked_output_color[0];
    p->color[1] = self->picked_output_color[1];
    p->color[2] = self->picked_output_color[2];
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->colorpick), &c);
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  return FALSE;
}

static void aspect_changed(GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;
  int which = dt_bauhaus_combobox_get(combo);
  const char *text = dt_bauhaus_combobox_get_text(combo);
  if(which < 0)
  {
    p->aspect = DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE;
    if(text)
    {
      const char *c = text;
      const char *end = text + strlen(text);
      while(*c != ':' && *c != '/' && c < end) c++;
      if(c < end - 1)
      {
        // *c = '\0'; // not needed, atof will stop there.
        c++;
        p->aspect = atof(text) / atof(c);
        g_strlcpy(p->aspect_text, text, sizeof(p->aspect_text));
      }
    }
  }
  else if(which < DT_IOP_BORDERS_ASPECT_COUNT)
  {
    g_strlcpy(p->aspect_text, text, sizeof(p->aspect_text));
    p->aspect = g->aspect_ratios[which];
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void aspect_orient_changed(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;
  p->aspect_orient = dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void position_h_changed(GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;
  int which = dt_bauhaus_combobox_get(combo);
  const char *text = dt_bauhaus_combobox_get_text(combo);
  if(which < 0)
  {
    p->pos_h = 0.5f; // center
    if(text)
    {
      const char *c = text;
      const char *end = text + strlen(text);
      while(*c != ':' && *c != '/' && c < end) c++;
      if(c < end - 1)
      {
        // *c = '\0'; // not needed, atof will stop there.
        c++;
        p->pos_h = atof(text) / atof(c);
      }
      else
      {
        p->pos_h = atof(text);
      }
      g_strlcpy(p->pos_h_text, text, sizeof(p->pos_h_text));
      p->pos_h = MAX(p->pos_h, 0);
      p->pos_h = MIN(p->pos_h, 1);
    }
  }
  else if(which < DT_IOP_BORDERS_POSITION_H_COUNT)
  {
    g_strlcpy(p->pos_h_text, text, sizeof(p->pos_h_text));
    p->pos_h = g->pos_h_ratios[which];
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void position_v_changed(GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;
  int which = dt_bauhaus_combobox_get(combo);
  const char *text = dt_bauhaus_combobox_get_text(combo);
  if(which < 0)
  {
    p->pos_v = 0.5f; // center
    if(text)
    {
      const char *c = text;
      const char *end = text + strlen(text);
      while(*c != ':' && *c != '/' && c < end) c++;
      if(c < end - 1)
      {
        // *c = '\0'; // not needed, atof will stop there.
        c++;
        p->pos_v = atof(text) / atof(c);
      }
      else
      {
        p->pos_v = atof(text);
      }
      g_strlcpy(p->pos_v_text, text, sizeof(p->pos_v_text));
      p->pos_v = MAX(p->pos_v, 0);
      p->pos_v = MIN(p->pos_v, 1);
    }
  }
  else if(which < DT_IOP_BORDERS_POSITION_H_COUNT)
  {
    g_strlcpy(p->pos_v_text, text, sizeof(p->pos_v_text));
    p->pos_v = g->pos_h_ratios[which];
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void size_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;
  p->size = dt_bauhaus_slider_get(slider) / 100.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void frame_size_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;
  p->frame_size = dt_bauhaus_slider_get(slider) / 100.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void frame_offset_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;
  p->frame_offset = dt_bauhaus_slider_get(slider) / 100.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void colorpick_color_set(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;

  // turn off the other color picker so that this tool actually works ...
  gtk_toggle_button_set_active(g->frame_picker, FALSE);
  gtk_toggle_button_set_active(g->border_picker, FALSE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  p->color[0] = c.red;
  p->color[1] = c.green;
  p->color[2] = c.blue;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void frame_colorpick_color_set(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;

  // turn off the other color picker so that this tool actually works ...
  gtk_toggle_button_set_active(g->frame_picker, FALSE);
  gtk_toggle_button_set_active(g->border_picker, FALSE);

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
  dt_bauhaus_slider_set(g->size, p->size * 100.0f);

  // ----- Aspect
  int k = 0;
  for(; k < DT_IOP_BORDERS_ASPECT_COUNT; k++)
  {
    if(fabsf(p->aspect - g->aspect_ratios[k]) < 0.0001f)
    {
      dt_bauhaus_combobox_set(g->aspect, k);
      break;
    }
  }
  if(k == DT_IOP_BORDERS_ASPECT_COUNT)
  {
    dt_bauhaus_combobox_set_text(g->aspect, p->aspect_text);
    dt_bauhaus_combobox_set(g->aspect, -1);
  }

  // ----- aspect orientation
  dt_bauhaus_combobox_set(g->aspect_orient, p->aspect_orient);

  // ----- Position H
  for(k = 0; k < DT_IOP_BORDERS_POSITION_H_COUNT; k++)
  {
    if(fabsf(p->pos_h - g->pos_h_ratios[k]) < 0.0001f)
    {
      dt_bauhaus_combobox_set(g->pos_h, k);
      break;
    }
  }
  if(k == DT_IOP_BORDERS_POSITION_H_COUNT)
  {
    dt_bauhaus_combobox_set_text(g->pos_h, p->pos_h_text);
    dt_bauhaus_combobox_set(g->pos_h, -1);
  }

  // ----- Position V
  for(k = 0; k < DT_IOP_BORDERS_POSITION_V_COUNT; k++)
  {
    if(fabsf(p->pos_v - g->pos_v_ratios[k]) < 0.0001f)
    {
      dt_bauhaus_combobox_set(g->pos_v, k);
      break;
    }
  }
  if(k == DT_IOP_BORDERS_POSITION_V_COUNT)
  {
    dt_bauhaus_combobox_set_text(g->pos_v, p->pos_v_text);
    dt_bauhaus_combobox_set(g->pos_v, -1);
  }


  dt_bauhaus_slider_set(g->frame_size, p->frame_size * 100.0f);
  dt_bauhaus_slider_set(g->frame_offset, p->frame_offset * 100.0f);

  // ----- Border Color
  GdkRGBA c = (GdkRGBA){.red = p->color[0], .green = p->color[1], .blue = p->color[2], .alpha = 1.0 };
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->colorpick), &c);

  // ----- Frame Color
  GdkRGBA fc = (GdkRGBA){
    .red = p->frame_color[0], .green = p->frame_color[1], .blue = p->frame_color[2], .alpha = 1.0
  };
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->frame_colorpick), &fc);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_borders_data_t));
  module->params = malloc(sizeof(dt_iop_borders_params_t));
  module->default_params = malloc(sizeof(dt_iop_borders_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_borders_params_t);
  module->gui_data = NULL;
  module->priority = 950; // module order created by iop_dependencies.py, do not edit!
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

static void gui_init_aspect(struct dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;

  dt_bauhaus_combobox_add(g->aspect, _("image"));
  dt_bauhaus_combobox_add(g->aspect, _("3:1"));
  dt_bauhaus_combobox_add(g->aspect, _("95:33"));
  dt_bauhaus_combobox_add(g->aspect, _("2:1"));
  dt_bauhaus_combobox_add(g->aspect, _("16:9"));
  dt_bauhaus_combobox_add(g->aspect, _("golden cut"));
  dt_bauhaus_combobox_add(g->aspect, _("3:2"));
  dt_bauhaus_combobox_add(g->aspect, _("A4"));
  dt_bauhaus_combobox_add(g->aspect, _("DIN"));
  dt_bauhaus_combobox_add(g->aspect, _("4:3"));
  dt_bauhaus_combobox_add(g->aspect, _("square"));
  dt_bauhaus_combobox_add(g->aspect, _("constant border"));

  g->aspect_ratios[DT_IOP_BORDERS_ASPECT_IMAGE_IDX] = DT_IOP_BORDERS_ASPECT_IMAGE_VALUE;
  g->aspect_ratios[DT_IOP_BORDERS_ASPECT_CONSTANT_IDX] = DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE;
  int i = 1;
  g->aspect_ratios[i++] = 3.0f;
  g->aspect_ratios[i++] = 95.0f / 33.0f;
  g->aspect_ratios[i++] = 2.0f;
  g->aspect_ratios[i++] = 16.0f / 9.0f;
  g->aspect_ratios[i++] = PHI;
  g->aspect_ratios[i++] = 3.0f / 2.0f;
  g->aspect_ratios[i++] = 297.0f / 210.0f;
  g->aspect_ratios[i++] = sqrtf(2.0f);
  g->aspect_ratios[i++] = 4.0f / 3.0f;
  g->aspect_ratios[i++] = 1.0f;
}

static void gui_init_positions(struct dt_iop_module_t *self)
{
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;

  dt_bauhaus_combobox_add(g->pos_h, _("center"));
  dt_bauhaus_combobox_add(g->pos_h, _("1/3"));
  dt_bauhaus_combobox_add(g->pos_h, _("3/8"));
  dt_bauhaus_combobox_add(g->pos_h, _("5/8"));
  dt_bauhaus_combobox_add(g->pos_h, _("2/3"));
  dt_bauhaus_combobox_add(g->pos_v, _("center"));
  dt_bauhaus_combobox_add(g->pos_v, _("1/3"));
  dt_bauhaus_combobox_add(g->pos_v, _("3/8"));
  dt_bauhaus_combobox_add(g->pos_v, _("5/8"));
  dt_bauhaus_combobox_add(g->pos_v, _("2/3"));

  int i = 0;
  g->pos_h_ratios[i++] = 0.5f;
  g->pos_h_ratios[i++] = 1.0f / 3.0f;
  g->pos_h_ratios[i++] = 3.0f / 8.0f;
  g->pos_h_ratios[i++] = 5.0f / 8.0f;
  g->pos_h_ratios[i++] = 2.0f / 3.0f;
  i = 0;
  g->pos_v_ratios[i++] = 0.5f;
  g->pos_v_ratios[i++] = 1.0f / 3.0f;
  g->pos_v_ratios[i++] = 3.0f / 8.0f;
  g->pos_v_ratios[i++] = 5.0f / 8.0f;
  g->pos_v_ratios[i++] = 2.0f / 3.0f;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_borders_gui_data_t));
  dt_iop_borders_gui_data_t *g = (dt_iop_borders_gui_data_t *)self->gui_data;
  dt_iop_borders_params_t *p = (dt_iop_borders_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->size = dt_bauhaus_slider_new_with_range(self, 0.0, 50.0, 0.5, p->size * 100.0, 2);
  dt_bauhaus_widget_set_label(g->size, NULL, _("border size"));
  dt_bauhaus_slider_set_format(g->size, "%.2f%%");
  g_signal_connect(G_OBJECT(g->size), "value-changed", G_CALLBACK(size_callback), self);
  g_object_set(G_OBJECT(g->size), "tooltip-text", _("size of the border in percent of the full image"),
               (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), g->size, TRUE, TRUE, 0);

  g->aspect = dt_bauhaus_combobox_new(self);
  dt_bauhaus_combobox_set_editable(g->aspect, 1);
  dt_bauhaus_widget_set_label(g->aspect, NULL, _("aspect"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->aspect, TRUE, TRUE, 0);
  gui_init_aspect(self);
  g_signal_connect(G_OBJECT(g->aspect), "value-changed", G_CALLBACK(aspect_changed), self);
  g_object_set(G_OBJECT(g->aspect), "tooltip-text",
               _("select the aspect ratio or right click and type your own (w:h)"), (char *)NULL);

  g->aspect_orient = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->aspect_orient, NULL, _("orientation"));
  dt_bauhaus_combobox_add(g->aspect_orient, _("auto"));
  dt_bauhaus_combobox_add(g->aspect_orient, _("portrait"));
  dt_bauhaus_combobox_add(g->aspect_orient, _("landscape"));
  g_object_set(G_OBJECT(g->aspect_orient), "tooltip-text",
               _("aspect ratio orientation of the image with border"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->aspect_orient), "value-changed", G_CALLBACK(aspect_orient_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->aspect_orient, TRUE, TRUE, 0);

  g->pos_h = dt_bauhaus_combobox_new(self);
  dt_bauhaus_combobox_set_editable(g->pos_h, 1);
  dt_bauhaus_widget_set_label(g->pos_h, NULL, _("horizontal position"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->pos_h, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->pos_h), "value-changed", G_CALLBACK(position_h_changed), self);
  g_object_set(
      G_OBJECT(g->pos_h), "tooltip-text",
      _("select the horizontal position ratio relative to top or right click and type your own (y:h)"),
      (char *)NULL);
  g->pos_v = dt_bauhaus_combobox_new(self);
  dt_bauhaus_combobox_set_editable(g->pos_v, 1);
  dt_bauhaus_widget_set_label(g->pos_v, NULL, _("vertical position"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->pos_v, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->pos_v), "value-changed", G_CALLBACK(position_v_changed), self);
  g_object_set(
      G_OBJECT(g->pos_v), "tooltip-text",
      _("select the vertical position ratio relative to left or right click and type your own (x:w)"),
      (char *)NULL);
  gui_init_positions(self);

  g->frame_size = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 0.5, p->frame_size * 100.0, 2);
  dt_bauhaus_widget_set_label(g->frame_size, NULL, _("frame line size"));
  dt_bauhaus_slider_set_format(g->frame_size, "%.2f%%");
  g_signal_connect(G_OBJECT(g->frame_size), "value-changed", G_CALLBACK(frame_size_callback), self);
  g_object_set(G_OBJECT(g->frame_size), "tooltip-text",
               _("size of the frame line in percent of min border width"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), g->frame_size, TRUE, TRUE, 0);

  g->frame_offset = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 0.5, p->frame_offset * 100.0, 2);
  dt_bauhaus_widget_set_label(g->frame_offset, NULL, _("frame line offset"));
  dt_bauhaus_slider_set_format(g->frame_offset, "%.2f%%");
  g_signal_connect(G_OBJECT(g->frame_offset), "value-changed", G_CALLBACK(frame_offset_callback), self);
  g_object_set(G_OBJECT(g->frame_offset), "tooltip-text",
               _("offset of the frame line beginning on picture side"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), g->frame_offset, TRUE, TRUE, 0);

  GdkRGBA color = (GdkRGBA){.red = p->color[0], .green = p->color[1], .blue = p->color[2], .alpha = 1.0 };

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  g->colorpick = gtk_color_button_new_with_rgba(&color);
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->colorpick), FALSE);
  gtk_widget_set_size_request(GTK_WIDGET(g->colorpick), DT_PIXEL_APPLY_DPI(24), DT_PIXEL_APPLY_DPI(24));
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->colorpick), _("select border color"));
  GtkWidget *label = dtgtk_reset_label_new(_("border color"), self, &p->color, 3 * sizeof(float));
  g_signal_connect(G_OBJECT(g->colorpick), "color-set", G_CALLBACK(colorpick_color_set), self);

  g->border_picker = GTK_TOGGLE_BUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT));
  g_object_set(G_OBJECT(g->border_picker), "tooltip-text", _("pick border color from image"), (char *)NULL);
  gtk_widget_set_size_request(GTK_WIDGET(g->border_picker), DT_PIXEL_APPLY_DPI(24), DT_PIXEL_APPLY_DPI(24));
  g_signal_connect(G_OBJECT(g->border_picker), "toggled", G_CALLBACK(request_pick_toggled_border), self);

  gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->colorpick), FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->border_picker), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), box, TRUE, TRUE, 0);

  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  g->frame_colorpick = gtk_color_button_new_with_rgba(&color);
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->frame_colorpick), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->frame_colorpick), _("select frame line color"));
  gtk_widget_set_size_request(GTK_WIDGET(g->frame_colorpick), DT_PIXEL_APPLY_DPI(24), DT_PIXEL_APPLY_DPI(24));
  label = dtgtk_reset_label_new(_("frame line color"), self, &p->color, 3 * sizeof(float));
  g_signal_connect(G_OBJECT(g->frame_colorpick), "color-set", G_CALLBACK(frame_colorpick_color_set), self);

  g->frame_picker = GTK_TOGGLE_BUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT));
  g_object_set(G_OBJECT(g->frame_picker), "tooltip-text", _("pick frame line color from image"), (char *)NULL);
  gtk_widget_set_size_request(GTK_WIDGET(g->frame_picker), DT_PIXEL_APPLY_DPI(24), DT_PIXEL_APPLY_DPI(24));
  g_signal_connect(G_OBJECT(g->frame_picker), "toggled", G_CALLBACK(request_pick_toggled_frame), self);

  gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->frame_colorpick), FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->frame_picker), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), box, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(borders_draw), self);
}


void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_borders_params_t tmp = (dt_iop_borders_params_t){ { 1.0f, 1.0f, 1.0f },
                                                           DT_IOP_BORDERS_ASPECT_CONSTANT_VALUE,
                                                           "constant border",
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
  memcpy(self->params, &tmp, sizeof(dt_iop_borders_params_t));
  memcpy(self->default_params, &tmp, sizeof(dt_iop_borders_params_t));
  self->default_enabled = 0;
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
