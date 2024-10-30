/*
    This file is part of darktable,
    Copyright (C) 2024 darktable developers.

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
// our includes go first:
#include "bauhaus/bauhaus.h"
#include "develop/imageop.h"
#include "develop/borders_helper.h"
#include "develop/imageop_gui.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(1, dt_iop_enlargecanvas_params_t)

typedef enum dt_iop_canvas_color_t
{
  DT_IOP_CANVAS_COLOR_GREEN = 0,  // $DESCRIPTION: "green"
  DT_IOP_CANVAS_COLOR_RED   = 1,  // $DESCRIPTION: "red"
  DT_IOP_CANVAS_COLOR_BLUE  = 2,  // $DESCRIPTION: "blue"
  DT_IOP_CANVAS_COLOR_BLACK = 3,  // $DESCRIPTION: "black"
  DT_IOP_CANVAS_COLOR_WHITE = 4,  // $DESCRIPTION: "white"
  DT_IOP_CANVAS_COLOR_COUNT = 5,
} dt_iop_canvas_color_t;

typedef struct dt_iop_enlargecanvas_params_t
{
  float percent_left;   // $MIN: 0 $MAX: 100.0 $DEFAULT: 0 $DESCRIPTION: "percent left"
  float percent_right;  // $MIN: 0 $MAX: 100.0 $DEFAULT: 0 $DESCRIPTION: "percent right"
  float percent_top;    // $MIN: 0 $MAX: 100.0 $DEFAULT: 0 $DESCRIPTION: "percent top"
  float percent_bottom; // $MIN: 0 $MAX: 100.0 $DEFAULT: 0 $DESCRIPTION: "percent bottom"
  dt_iop_canvas_color_t color;  // $DESCRIPTION: "color"
} dt_iop_enlargecanvas_params_t;

typedef struct dt_iop_enlargecanvas_data_t
{
  float percent_left;
  float percent_right;
  float percent_top;
  float percent_bottom;
  dt_iop_canvas_color_t color;
} dt_iop_enlargecanvas_data_t;

typedef struct dt_iop_enlargecanvas_gui_data_t
{
  GtkWidget *percent_left;
  GtkWidget *percent_right;
  GtkWidget *percent_top;
  GtkWidget *percent_bottom;
  GtkWidget *color;
} dt_iop_enlargecanvas_gui_data_t;

const char *name()
{
  return _("enlarge canvas");
}

const char** description(dt_iop_module_t *self)
{
  return dt_iop_set_description
    (self,
     _("add empty space to the left, top, right or bottom"),
     _("corrective and creative"),
     _("linear, RGB, scene-referred"),
     _("linear, RGB"),
     _("linear, RGB, scene-referred"));
}

const char *aliases()
{
  return _("composition|expand|extend");
}

int flags()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_EFFECTS;
}

// where does it appear in the gui?
int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, p1, self->params_size);
}

void modify_roi_out(dt_iop_module_t *self,
                    dt_dev_pixelpipe_iop_t *piece,
                    dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *roi_in)
{
  dt_iop_enlargecanvas_data_t *d = piece->data;
  *roi_out = *roi_in;

  const int border_size_l = roi_in->width * d->percent_left / 100.f;
  const int border_size_r = roi_in->width * d->percent_right / 100.f;
  const int border_size_t = roi_in->height * d->percent_top / 100.f;
  const int border_size_b = roi_in->height * d->percent_bottom / 100.f;

  if(border_size_l > 0)
  {
    roi_out->width += border_size_l;
  }
  if(border_size_r > 0)
  {
    roi_out->width += border_size_r;
  }
  if(border_size_t > 0)
  {
    roi_out->height += border_size_t;
  }
  if(border_size_b > 0)
  {
    roi_out->height += border_size_b;
  }

  roi_out->width = CLAMP(roi_out->width, 5, roi_in->width * 3);
  roi_out->height = CLAMP(roi_out->height, 5, roi_in->height * 3);
}

void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out,
                   dt_iop_roi_t *roi_in)
{
  dt_iop_enlargecanvas_data_t *d = piece->data;
  *roi_in = *roi_out;

  const float bw = (piece->buf_out.width - piece->buf_in.width) * roi_out->scale;
  const float bh = (piece->buf_out.height - piece->buf_in.height) * roi_out->scale;

  float pl = .0f;
  float pt = .0f;

  if(d->percent_left > 0.f)
    pl = d->percent_left / (d->percent_left + d->percent_right);
  if(d->percent_top > 0.f)
    pt = d->percent_top / (d->percent_top + d->percent_bottom);

  const int border_size_l = bw * pl;
  const int border_size_t = bh * pt;

  // don't request outside image (no px for borders)
  roi_in->x = MAX(roundf(roi_out->x - border_size_l), 0);
  roi_in->y = MAX(roundf(roi_out->y - border_size_t), 0);

  // subtract upper left border from dimensions
  roi_in->width -= MAX(roundf(border_size_l - roi_out->x), 0);
  roi_in->height -= MAX(roundf(border_size_t - roi_out->y), 0);

  // subtract lower right border from dimensions
  const float p_inw = (float)piece->buf_in.width * roi_out->scale;
  const float p_inh = (float)piece->buf_in.height * roi_out->scale;

  roi_in->width  -= MAX(roundf((float)(roi_in->x + roi_in->width) - p_inw), 0);
  roi_in->height -= MAX(roundf((float)(roi_in->y + roi_in->height) - p_inh), 0);

  // sanity check: don't request nothing or outside roi
  roi_in->width = MIN(p_inw, MAX(1, roi_in->width));
  roi_in->height = MIN(p_inh, MAX(1, roi_in->height));
}

/** modify pixel coordinates according to the pixel shifts the module
 * applies (optional, per-pixel ops don't need) */
int distort_transform(dt_iop_module_t *self,
                      dt_dev_pixelpipe_iop_t *piece,
                      float *points,
                      const size_t points_count)
{
  const dt_iop_enlargecanvas_params_t *d = piece->data;

  const int bw = (piece->buf_out.width - piece->buf_in.width);
  const int bh = (piece->buf_out.height - piece->buf_in.height);

  float pl = .0f;
  float pt = .0f;

  if(d->percent_left > 0.f)
    pl = d->percent_left / (d->percent_left + d->percent_right);
  if(d->percent_top > 0.f)
    pt = d->percent_top / (d->percent_top + d->percent_bottom);

  const int border_size_l = bw * pl;
  const int border_size_t = bh * pt;

  if(border_size_l > 0 || border_size_t > 0)
  {
    // apply the coordinate adjustment to each provided point
    float *const pts = DT_IS_ALIGNED(points);

    DT_OMP_FOR(if(points_count > 100))
    for(size_t i = 0; i < points_count * 2; i += 2)
    {
      pts[i] += border_size_l;
      pts[i + 1] += border_size_t;
    }
  }

  return 1;  // return 1 on success, 0 if one or more points could not be transformed
}

int distort_backtransform(dt_iop_module_t *self,
                          dt_dev_pixelpipe_iop_t *piece,
                          float *points,
                          size_t points_count)
{
  const dt_iop_enlargecanvas_params_t *d = piece->data;

  const int bw = (piece->buf_out.width - piece->buf_in.width);
  const int bh = (piece->buf_out.height - piece->buf_in.height);

  float pl = .0f;
  float pt = .0f;

  if(d->percent_left > 0.f)
    pl = d->percent_left / (d->percent_left + d->percent_right);
  if(d->percent_top > 0.f)
    pt = d->percent_top / (d->percent_top + d->percent_bottom);

  const int border_size_l = bw * pl;
  const int border_size_t = bh * pt;

  if (border_size_l > 0 || border_size_t > 0)
  {
    float *const pts = DT_IS_ALIGNED(points);

    DT_OMP_FOR(if(points_count > 100))
    for(size_t i = 0; i < points_count * 2; i += 2)
    {
      pts[i] -= border_size_l;
      pts[i + 1] -= border_size_t;
    }
  }

  return 1;  // return 1 on success, 0 if one or more points could not be back-transformed
}

static void _compute_pos(const dt_iop_enlargecanvas_data_t *const d,
                         float *pos_v,
                         float *pos_h)
{
  *pos_v = .5f;
  *pos_h = .5f;

  const gboolean has_left   = d->percent_left > 0.f;
  const gboolean has_right  = d->percent_right > 0.f;
  const gboolean has_top    = d->percent_top > 0.f;
  const gboolean has_bottom = d->percent_bottom > 0.f;

  if(has_right || has_left)
    *pos_h = d->percent_left / (d->percent_left + d->percent_right);

  if(has_top || has_bottom)
    *pos_v = d->percent_top / (d->percent_top + d->percent_bottom);

  *pos_v = CLAMP(*pos_v, 0.0f, 1.0f);
  *pos_h = CLAMP(*pos_h, 0.0f, 1.0f);
}

void distort_mask(dt_iop_module_t *self,
                  dt_dev_pixelpipe_iop_t *piece,
                  const float *const in,
                  float *const out,
                  const dt_iop_roi_t *const roi_in,
                  const dt_iop_roi_t *const roi_out)
{
  const dt_iop_enlargecanvas_data_t *const d = piece->data;

  float pos_v = .5f;
  float pos_h = .5f;

  _compute_pos(d, &pos_v, &pos_h);

  dt_iop_border_positions_t binfo;

  float bcolor[4] = { 0 };
  float fcolor[4] = { 0 };

  dt_iop_setup_binfo(piece, roi_in, roi_out, pos_v, pos_h,
                     bcolor, fcolor, 0.f, 0.f, &binfo);

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

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_enlargecanvas_data_t *const d = piece->data;

  float pos_v = .5f;
  float pos_h = .5f;

  _compute_pos(d, &pos_v, &pos_h);

  float fcolor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
  float bcolor[4];

  bcolor[3] = 1.0f;

  // default is green, check for alternatives
  switch(d->color)
  {
      case DT_IOP_CANVAS_COLOR_BLACK:
        bcolor[0] = 0.f;
        bcolor[1] = 0.f;
        bcolor[2] = 0.f;
        break;
      case DT_IOP_CANVAS_COLOR_WHITE:
        bcolor[0] = 1.f;
        bcolor[1] = 1.f;
        bcolor[2] = 1.f;
        break;
      case DT_IOP_CANVAS_COLOR_RED:
        bcolor[0] = 1.f;
        bcolor[1] = 0.f;
        bcolor[2] = 0.f;
        break;
      case DT_IOP_CANVAS_COLOR_GREEN:
        bcolor[0] = 0.f;
        bcolor[1] = 1.f;
        bcolor[2] = 0.f;
        break;
      case DT_IOP_CANVAS_COLOR_BLUE:
        bcolor[0] = 0.f;
        bcolor[1] = 0.f;
        bcolor[2] = 1.f;
        break;
      case DT_IOP_CANVAS_COLOR_COUNT:
        // should never happen
        break;
  }

  dt_iop_border_positions_t binfo;

  dt_iop_setup_binfo(piece, roi_in, roi_out, pos_v, pos_h,
                     bcolor, fcolor, 0.f, 0.f, &binfo);

  dt_iop_copy_image_with_border((float*)ovoid, (const float*)ivoid, &binfo);
}

void cleanup(dt_iop_module_t *self)
{
  free(self->params);
  self->params = NULL;
  free(self->default_params);
  self->default_params = NULL;
}

void cleanup_global(dt_iop_module_so_t *self)
{
  free(self->data);
  self->data = NULL;
}

/** gui setup and update, these are needed. */
void gui_update(dt_iop_module_t *self)
{
  dt_iop_enlargecanvas_gui_data_t *g = self->gui_data;
  dt_iop_enlargecanvas_params_t *p = self->params;

  dt_bauhaus_slider_set(g->percent_left, p->percent_left);
  dt_bauhaus_slider_set(g->percent_right, p->percent_right);
  dt_bauhaus_slider_set(g->percent_top, p->percent_top);
  dt_bauhaus_slider_set(g->percent_bottom, p->percent_bottom);
  dt_bauhaus_combobox_set(g->color, p->color);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_enlargecanvas_gui_data_t *g = IOP_GUI_ALLOC(enlargecanvas);

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->percent_left = dt_bauhaus_slider_from_params(self, "percent_left");
  dt_bauhaus_slider_set_format(g->percent_left, "%");
  gtk_widget_set_tooltip_text(g->percent_left,
                              _("how much to enlarge the canvas to the left "
                              "as a percentage of the original image width"));

  g->percent_right = dt_bauhaus_slider_from_params(self, "percent_right");
  dt_bauhaus_slider_set_format(g->percent_right, "%");
  gtk_widget_set_tooltip_text(g->percent_right,
                              _("how much to enlarge the canvas to the right "
                              "as a percentage of the original image width"));

  g->percent_top = dt_bauhaus_slider_from_params(self, "percent_top");
  dt_bauhaus_slider_set_format(g->percent_top, "%");
  gtk_widget_set_tooltip_text(g->percent_top,
                              _("how much to enlarge the canvas to the top "
                              "as a percentage of the original image height"));

  g->percent_bottom = dt_bauhaus_slider_from_params(self, "percent_bottom");
  dt_bauhaus_slider_set_format(g->percent_bottom, "%");
  gtk_widget_set_tooltip_text(g->percent_bottom,
                              _("how much to enlarge the canvas to the bottom "
                              "as a percentage of the original image height"));

  g->color = dt_bauhaus_combobox_from_params(self, "color");
  gtk_widget_set_tooltip_text(g->color, _("select the color of the enlarged canvas"));
}

void gui_cleanup(dt_iop_module_t *self)
{
  IOP_GUI_FREE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
