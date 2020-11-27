/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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
#include "common/interpolation.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>

#define NKNOTS 16

DT_MODULE_INTROSPECTION(1, dt_iop_mlens_params_t)

typedef struct dt_iop_mlens_params_t
{
  gboolean cor_dist;  // $DEFAULT: 1 $DESCRIPTION: "correct distortion"
  gboolean cor_ca;    // $DEFAULT: 1 $DESCRIPTION: "correct chromatic aberration"
  gboolean cor_vig;   // $DEFAULT: 1 $DESCRIPTION: "correct vignetting"
  float cor_dist_ft;  // $DEFAULT: 1 $MIN: 0 $MAX: 2 $DESCRIPTION: "distortion fine-tune"
  float cor_vig_ft;   // $DEFAULT: 1 $MIN: 0 $MAX: 2 $DESCRIPTION: "vignette fine-tune"
  float scale;
} dt_iop_mlens_params_t;

typedef struct dt_iop_mlens_gui_data_t
{
  GtkWidget *cor_dist, *cor_ca, *cor_vig, *cor_dist_ft, *cor_vig_ft;
} dt_iop_mlens_gui_data_t;

const char *name()
{
  return _("manufacturer lens correction");
}

const char *aliases()
{
  return _("vignette|chromatic aberrations|distortion");
}

const char *description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("correct lenses optical flaws"),
                                _("corrective"),
                                _("linear, RGB, scene-referred"),
                                _("geometric and reconstruction, RGB"),
                                _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_TECHNICAL;
}

int operation_tags()
{
  return IOP_TAG_DISTORT;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_UNSAFE_COPY;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

static int init_coeffs(const dt_image_t *img, const dt_iop_mlens_params_t *d,
                       float knots[NKNOTS], float cor_rgb[3][NKNOTS], float vig[NKNOTS])
{
  const dt_image_correction_data_t *cd = &img->exif_correction_data;

  if(img->exif_correction_type == CORRECTION_TYPE_SONY)
  {
    int nc = cd->sony.nc;
    for(int i = 0; i < nc; i++)
    {
      knots[i] = (float) i / (nc - 1);

      if(cor_rgb && d->cor_dist)
        cor_rgb[0][i] = cor_rgb[1][i] = cor_rgb[2][i] = (d->cor_dist_ft * cd->sony.distortion[i] * powf(2, -14) + 1) * d->scale;
      else if(cor_rgb)
        cor_rgb[0][i] = cor_rgb[1][i] = cor_rgb[2][i] = d->scale;

      if(cor_rgb && d->cor_ca)
      {
        cor_rgb[0][i] *= cd->sony.ca_r[i] * pow(2, -21) + 1;
        cor_rgb[2][i] *= cd->sony.ca_b[i] * pow(2, -21) + 1;
      }

      if(vig && d->cor_vig)
        vig[i] = powf(2, 0.5f - powf(2, d->cor_vig_ft * cd->sony.vignetting[i] * powf(2, -13)  - 1));
      else if(vig)
        vig[i] = 1;
    }

    return nc;
  }
  else if(img->exif_correction_type == CORRECTION_TYPE_FUJI)
  {
    for(int i = 0; i < 9; i++)
    {
      knots[i] = cd->fuji.cropf * cd->fuji.knots[i];

      if(cor_rgb && d->cor_dist)
        cor_rgb[0][i] = cor_rgb[1][i] = cor_rgb[2][i] = (d->cor_dist_ft * cd->fuji.distortion[i] / 100 + 1) * d->scale;
      else if(cor_rgb)
        cor_rgb[0][i] = cor_rgb[1][i] = cor_rgb[2][i] = d->scale;

      if(cor_rgb && d->cor_ca)
      {
        cor_rgb[0][i] *= cd->fuji.ca_r[i] + 1;
        cor_rgb[2][i] *= cd->fuji.ca_b[i] + 1;
      }

      if(vig && d->cor_vig)
        vig[i] = 1 - d->cor_vig_ft * (1 - cd->fuji.vignetting[i] / 100);
      else if(vig)
        vig[i] = 1;
    }

    return 9;
  }

  return 0;
}

static float interpolate(const float *xi, const float *yi, int ni, float x)
{
  if(x < xi[0])
    return yi[0];

  for(int i = 1; i < ni; i++)
  {
    if(x >= xi[i - 1] && x <= xi[i])
    {
      float dydx = (yi[i] - yi[i - 1]) / (xi[i] - xi[i - 1]);

      return yi[i - 1] + (x - xi[i - 1]) * dydx;
    }
  }

  return yi[ni - 1];
}

static void auto_scale(const dt_image_t *img, dt_iop_mlens_params_t *p)
{
  // Default the scale to one for the benefit of init_coeffs
  p->scale = 1;

  float knots[NKNOTS], cor_rgb[3][NKNOTS];
  int nc = init_coeffs(img, p, knots, cor_rgb, NULL);

  // Compute the new scale
  float scale = 0;
  for(int i = 0; i < 200; i++)
    for(int j = 0; j < 3; j++)
      scale = MAX(scale, interpolate(knots, cor_rgb[j], nc, 0.5 + 0.5*i/(200 - 1)));

  p->scale = 1 / scale;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_mlens_params_t *p = p1, *q = piece->data;

  *q = *p;
  auto_scale(&self->dev->image_storage, q);
}

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 4.5f; // in + out + tmp + tmpbuf
  tiling->maxbuf = 1.5f;
  tiling->overhead = 0;
  tiling->overlap = 4;
  tiling->xalign = 1;
  tiling->yalign = 1;
}

int distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points, size_t points_count)
{
  const dt_image_t *img = &self->dev->image_storage;
  const dt_iop_mlens_params_t *d = piece->data;

  // Prepare the correction splines
  float knots[NKNOTS], cor_rgb[3][NKNOTS];
  int nc = init_coeffs(img, d, knots, cor_rgb, NULL);

  const float w2 = 0.5f * piece->buf_in.width;
  const float h2 = 0.5f * piece->buf_in.height;
  const float r = 1 / sqrtf(w2*w2 + h2*h2);

  for(size_t i = 0; i < 2*points_count; i += 2)
  {
    float p1 = points[i];
    float p2 = points[i + 1];

    for(int k = 0; k < 10; k++)
    {
      float cx = p1 - w2, cy = p2 - h2;
      float dr = interpolate(knots, cor_rgb[1], nc, r*sqrtf(cx*cx + cy*cy));

      float dist1 = points[i] - (dr*cx + w2), dist2 = points[i + 1] - (dr*cy + h2);
      if(fabs(dist1) < .5f && fabs(dist2) < .5f)
        break;

      p1 += dist1;
      p2 += dist2;
    }

    points[i] = p1;
    points[i + 1] = p2;
  }

  return 1;
}

int distort_backtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points, size_t points_count)
{
  const dt_image_t *img = &self->dev->image_storage;
  const dt_iop_mlens_params_t *d = piece->data;

  // Prepare the correction splines
  float knots[NKNOTS], cor_rgb[3][NKNOTS];
  int nc = init_coeffs(img, d, knots, cor_rgb, NULL);

  const float w2 = 0.5f * piece->buf_in.width;
  const float h2 = 0.5f * piece->buf_in.height;
  const float r = 1 / sqrtf(w2*w2 + h2*h2);

  for(size_t i = 0; i < 2*points_count; i += 2)
  {
    float cx = points[i] - w2, cy = points[i + 1] - h2;
    float dr = interpolate(knots, cor_rgb[1], nc, r*sqrtf(cx*cx + cy*cy));

    points[i] = dr*cx + w2;
    points[i + 1] = dr*cy + h2;
  }

  return 1;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_image_t *img = &self->dev->image_storage;
  const dt_iop_mlens_params_t *d = piece->data;

  // Prepare the correction splines
  float knots[NKNOTS], cor_rgb[3][NKNOTS], vig[NKNOTS];
  int nc = init_coeffs(img, d, knots, cor_rgb, vig);

  const int ch = piece->colors;
  const int ch_width = ch * roi_in->width;
  const float w2 = 0.5f * roi_in->scale * piece->buf_in.width;
  const float h2 = 0.5f * roi_in->scale * piece->buf_in.height;
  const float r = 1 / sqrtf(w2*w2 + h2*h2);

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);

  // Allocate temporary storage
  const size_t bufsize = (size_t) roi_in->width * roi_in->height * ch * sizeof(float);
  float *buf = dt_alloc_align(64, bufsize);
  memcpy(buf, ivoid, bufsize);

  // Correct vignetting
  if(d->cor_vig)
  {
#ifdef _OPENMP
    #pragma omp parallel for
#endif
    for(int y = 0; y < roi_in->height; y++)
    {
      for(int x = 0; x < roi_in->width; x++)
      {
        float cx = roi_in->x + x - w2, cy = roi_in->y + y - h2;
        float sf = interpolate(knots, vig, nc, r*sqrtf(cx*cx + cy*cy));

        for(int c = 0; c < ch; c++)
          buf[y*ch_width + x*ch + c] /= sf*sf;
      }
    }
  }

  // Correct distortion and/or chromatic aberration
#ifdef _OPENMP
  #pragma omp parallel for
#endif
  for(int y = 0; y < roi_out->height; y++)
  {
    float *out = ((float *) ovoid) + (size_t) y * roi_out->width * ch;
    for(int x = 0; x < roi_out->width; x++, out += ch)
    {
      float cx = roi_out->x + x - w2, cy = roi_out->y + y - h2;

      for(int c = 0; c < ch; c++)
      {
        float dr = interpolate(knots, cor_rgb[c], nc, r*sqrtf(cx*cx + cy*cy));
        float xs = dr*cx + w2 - roi_in->x , ys = dr*cy + h2 - roi_in->y;
        out[c] = dt_interpolation_compute_sample(interpolation, buf + c, xs, ys, roi_in->width,
                                                 roi_in->height, ch, ch_width);
      }
    }
  }

  dt_free_align(buf);
}

void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *const roi_out, dt_iop_roi_t *roi_in)
{
  const dt_image_t *img = &self->dev->image_storage;
  const dt_iop_mlens_params_t *d = piece->data;

  float knots[NKNOTS], cor_rgb[3][NKNOTS];
  int nc = init_coeffs(img, d, knots, cor_rgb, NULL);

  *roi_in = *roi_out;

  const float orig_w = roi_in->scale * piece->buf_in.width;
  const float orig_h = roi_in->scale * piece->buf_in.height;
  const float w2 = 0.5f * orig_w, h2 = 0.5f * orig_h;
  const float r = 1 / sqrtf(w2*w2 + h2*h2);

  const int xoff = roi_in->x, yoff = roi_in->y;
  const int width = roi_in->width, height = roi_in->height;
  const float cxs[] = { xoff - w2, xoff + (width - 1) - w2 };
  const float cys[] = { yoff - h2, yoff + (height - 1) - h2 };

  float xm = FLT_MAX, xM = -FLT_MAX, ym = FLT_MAX, yM = -FLT_MAX;

  // Sweep along the top and bottom rows of the ROI
  for(int i = 0; i < width; i++)
  {
    float cx = xoff + i - w2;
    for(int j = 0; j < 2; j++)
    {
      float cy = cys[j], dr = 0;
      for(int c = 0; c < 3; c++)
        dr = MAX(dr, interpolate(knots, cor_rgb[c], nc, r*sqrtf(cx*cx + cy*cy)));
      float xs = dr*cx + w2, ys = dr*cy + h2;
      xm = MIN(xm, xs); xM = MAX(xM, xs); ym = MIN(ym, ys); yM = MAX(yM, ys);
    }
  }

  // Sweep along the left and right columns of the ROI
  for(int j = 0; j < height; j++)
  {
    float cy = yoff + j - h2;
    for(int i = 0; i < 2; i++)
    {
      float cx = cxs[i], dr = 0;
      for(int c = 0; c < 3; c++)
        dr = MAX(dr, interpolate(knots, cor_rgb[c], nc, r*sqrtf(cx*cx + cy*cy)));
      float xs = dr*cx + w2, ys = dr*cy + h2;
      xm = MIN(xm, xs); xM = MAX(xM, xs); ym = MIN(ym, ys); yM = MAX(yM, ys);
    }
  }

  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
  roi_in->x = fmaxf(0, xm - interpolation->width);
  roi_in->y = fmaxf(0, ym - interpolation->width);
  roi_in->width = fminf(orig_w - roi_in->x, xM - roi_in->x + interpolation->width);
  roi_in->height = fminf(orig_h - roi_in->y, yM - roi_in->y + interpolation->width);
}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);

  module->hide_enable_button = 1;
}

void reload_defaults(dt_iop_module_t *module)
{
  const dt_image_t *img = &module->dev->image_storage;
  dt_iop_mlens_params_t *p = module->default_params;

  p->cor_dist = 1;
  p->cor_ca = 1;
  p->cor_vig = 1;
  p->cor_dist_ft = 1;
  p->cor_vig_ft = 1;

  // Only admit if we have correction data available
  module->hide_enable_button = !img->exif_correction_type;

  if(module->widget)
    gtk_stack_set_visible_child_name(GTK_STACK(module->widget),
                                     module->hide_enable_button ? "unsupported" : "supported");

}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_mlens_gui_data_t *g = self->gui_data;
  dt_iop_mlens_params_t *p = self->params;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->cor_dist), p->cor_dist);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->cor_ca), p->cor_ca);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->cor_vig), p->cor_vig);
  dt_bauhaus_slider_set(g->cor_dist_ft, p->cor_dist_ft);
  dt_bauhaus_slider_set(g->cor_vig_ft, p->cor_vig_ft);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_mlens_gui_data_t *g = IOP_GUI_ALLOC(mlens);

  GtkWidget *box_supported = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  g->cor_dist = dt_bauhaus_toggle_from_params(self, "cor_dist");
  g->cor_ca = dt_bauhaus_toggle_from_params(self, "cor_ca");
  g->cor_vig = dt_bauhaus_toggle_from_params(self, "cor_vig");
  g->cor_dist_ft = dt_bauhaus_slider_from_params(self, "cor_dist_ft");
  g->cor_vig_ft = dt_bauhaus_slider_from_params(self, "cor_vig_ft");

  GtkWidget *label_unsupported = dt_ui_label_new(_("unsupported file type"));

  self->widget = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(self->widget), FALSE);
  gtk_stack_add_named(GTK_STACK(self->widget), label_unsupported, "unsupported");
  gtk_stack_add_named(GTK_STACK(self->widget), box_supported, "supported");
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
