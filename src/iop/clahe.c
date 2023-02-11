/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/math.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "dtgtk/resetlabel.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define ROUND_POSISTIVE(f) ((unsigned int)((f)+0.5))

DT_MODULE(1)

typedef struct dt_iop_rlce_params_t
{
  double radius;
  double slope;
} dt_iop_rlce_params_t;

typedef struct dt_iop_rlce_gui_data_t
{
  GtkBox *vbox1, *vbox2;
  GtkWidget *label1, *label2;
  GtkWidget *scale1, *scale2; // radie pixels, slope
} dt_iop_rlce_gui_data_t;

typedef struct dt_iop_rlce_data_t
{
  double radius;
  double slope;
} dt_iop_rlce_data_t;


const char *name()
{
  return _("Local contrast");
}

int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_EFFECTS;
}

const char *deprecated_msg()
{
  return _("This module is deprecated. Better use new local contrast module instead.");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_DEPRECATED;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_rlce_data_t *data = (dt_iop_rlce_data_t *)piece->data;
  const int ch = piece->colors;

  // PASS1: Get a luminance map of image...
  float *luminance = (float *)malloc(sizeof(float) * ((size_t)roi_out->width * roi_out->height));
// double lsmax=0.0,lsmin=1.0;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, ivoid, roi_out) \
  shared(luminance) \
  schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    float *in = (float *)ivoid + (size_t)j * roi_out->width * ch;
    float *lm = luminance + (size_t)j * roi_out->width;
    for(int i = 0; i < roi_out->width; i++)
    {
      double pmax = CLIP(fmax(in[0], fmax(in[1], in[2]))); // Max value in RGB set
      double pmin = CLIP(fmin(in[0], fmin(in[1], in[2]))); // Min value in RGB set
      *lm = (pmax + pmin) / 2.0;                           // Pixel luminocity
      in += ch;
      lm++;
    }
  }


  // Params
  const int rad = data->radius * roi_in->scale / piece->iscale;

#define BINS (256)

  const float slope = data->slope;

  size_t destbuf_size;
  float *const restrict dest_buf = dt_alloc_perthread_float(roi_out->width, &destbuf_size);

// CLAHE
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, dest_buf, destbuf_size, ivoid, ovoid, rad, roi_in, \
                      roi_out, slope) \
  shared(luminance) \
  schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    int yMin = fmax(0, j - rad);
    int yMax = fmin(roi_in->height, j + rad + 1);
    int h = yMax - yMin;

    int xMin0 = fmax(0, 0 - rad);
    int xMax0 = fmin(roi_in->width - 1, rad);

    int hist[BINS + 1];
    int clippedhist[BINS + 1];

    float *dest = dt_get_perthread(dest_buf, destbuf_size);

    /* initially fill histogram */
    memset(hist, 0, sizeof(int) * (BINS + 1));
    for(int yi = yMin; yi < yMax; ++yi)
      for(int xi = xMin0; xi < xMax0; ++xi)
        ++hist[ROUND_POSISTIVE(luminance[(size_t)yi * roi_in->width + xi] * (float)BINS)];

    // Destination row
    memset(dest, 0, sizeof(float) * roi_out->width);
    float *ld = dest;

    for(int i = 0; i < roi_out->width; i++)
    {

      int v = ROUND_POSISTIVE(luminance[(size_t)j * roi_in->width + i] * (float)BINS);

      int xMin = fmax(0, i - rad);
      int xMax = i + rad + 1;
      int w = fmin(roi_in->width, xMax) - xMin;
      int n = h * w;

      int limit = (int)(slope * n / BINS + 0.5f);

      /* remove left behind values from histogram */
      if(xMin > 0)
      {
        int xMin1 = xMin - 1;
        for(int yi = yMin; yi < yMax; ++yi)
          --hist[ROUND_POSISTIVE(luminance[(size_t)yi * roi_in->width + xMin1] * (float)BINS)];
      }

      /* add newly included values to histogram */
      if(xMax <= roi_in->width)
      {
        int xMax1 = xMax - 1;
        for(int yi = yMin; yi < yMax; ++yi)
          ++hist[ROUND_POSISTIVE(luminance[(size_t)yi * roi_in->width + xMax1] * (float)BINS)];
      }

      /* clip histogram and redistribute clipped entries */
      memcpy(clippedhist, hist, sizeof(int) * (BINS + 1));
      int ce = 0, ceb = 0;
      do
      {
        ceb = ce;
        ce = 0;
        for(int b = 0; b <= BINS; b++)
        {
          int d = clippedhist[b] - limit;
          if(d > 0)
          {
            ce += d;
            clippedhist[b] = limit;
          }
        }

        int d = (ce / (float)(BINS + 1));
        int m = ce % (BINS + 1);
        for(int b = 0; b <= BINS; b++) clippedhist[b] += d;

        if(m != 0)
        {
          int s = BINS / (float)m;
          for(int b = 0; b <= BINS; b += s) ++clippedhist[b];
        }
      } while(ce != ceb);

      /* build cdf of clipped histogram */
      unsigned int hMin = BINS;
      for(int b = 0; b < hMin; b++)
        if(clippedhist[b] != 0) hMin = b;

      int cdf = 0;
      for(int b = hMin; b <= v; b++) cdf += clippedhist[b];

      int cdfMax = cdf;
      for(int b = v + 1; b <= BINS; b++) cdfMax += clippedhist[b];

      int cdfMin = clippedhist[hMin];

      *ld = (cdf - cdfMin) / (float)(cdfMax - cdfMin);

      ld++;
    }

    // Apply row
    float *in = ((float *)ivoid) + (size_t)j * roi_out->width * ch;
    float *out = ((float *)ovoid) + (size_t)j * roi_out->width * ch;
    for(int r = 0; r < roi_out->width; r++)
    {
      float H, S, L;
      rgb2hsl(in, &H, &S, &L);
      // hsl2rgb(out,H,S,( L / dest[r] ) * (L-lsmin) + lsmin );
      hsl2rgb(out, H, S, dest[r]);
      out += ch;
      in += ch;
      ld++;
    }
  }

  dt_free_align(dest_buf);

  // Cleanup
  free(luminance);

#undef BINS
}

static void radius_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_rlce_params_t *p = (dt_iop_rlce_params_t *)self->params;
  p->radius = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void slope_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_rlce_params_t *p = (dt_iop_rlce_params_t *)self->params;
  p->slope = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}



void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rlce_params_t *p = (dt_iop_rlce_params_t *)p1;
  dt_iop_rlce_data_t *d = (dt_iop_rlce_data_t *)piece->data;

  d->radius = p->radius;
  d->slope = p->slope;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_rlce_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_rlce_gui_data_t *g = (dt_iop_rlce_gui_data_t *)self->gui_data;
  dt_iop_rlce_params_t *p = (dt_iop_rlce_params_t *)self->params;
  dt_bauhaus_slider_set(g->scale1, p->radius);
  dt_bauhaus_slider_set(g->scale2, p->slope);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_rlce_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_rlce_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_rlce_params_t);
  module->gui_data = NULL;
  *((dt_iop_rlce_params_t *)module->default_params) = (dt_iop_rlce_params_t){ 64, 1.25 };
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
  free(module->default_params);
  module->default_params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_rlce_gui_data_t *g = IOP_GUI_ALLOC(rlce);
  dt_iop_rlce_params_t *p = (dt_iop_rlce_params_t *)self->default_params;

  self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));

  g->vbox1 = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  g->vbox2 = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 0);

  g->label1 = dtgtk_reset_label_new(_("Radius"), self, &p->radius, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), g->label1, TRUE, TRUE, 0);
  g->label2 = dtgtk_reset_label_new(_("Amount"), self, &p->slope, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), g->label2, TRUE, TRUE, 0);

  g->scale1 = dt_bauhaus_slider_new_with_range(NULL, 0.0, 256.0, 0, p->radius, 0);
  g->scale2 = dt_bauhaus_slider_new_with_range(NULL, 1.0, 3.0, 0, p->slope, 2);
  // dtgtk_slider_set_format_type(g->scale2,DARKTABLE_SLIDER_FORMAT_PERCENT);

  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->scale1), _("Size of features to preserve"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->scale2), _("Strength of the effect"));

  g_signal_connect(G_OBJECT(g->scale1), "value-changed", G_CALLBACK(radius_callback), self);
  g_signal_connect(G_OBJECT(g->scale2), "value-changed", G_CALLBACK(slope_callback), self);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
