/*
    This file is part of darktable,
    Copyright (C) 2010-2025 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/math.h"
#include "control/control.h"
#include "common/dttypes.h"
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
  GtkWidget *scale1, *scale2; // radie pixels, slope
} dt_iop_rlce_gui_data_t;

typedef struct dt_iop_rlce_data_t
{
  double radius;
  double slope;
} dt_iop_rlce_data_t;


const char *name()
{
  return _("old local contrast");
}

int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_EFFECTS;
}

const char *deprecated_msg()
{
  return _("this module is deprecated. better use new local contrast module instead.");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_DEPRECATED;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  dt_iop_rlce_data_t *data = piece->data;
  const int ch = piece->colors;

  // PASS1: Get a luminance map of image...
  float *luminance = malloc(sizeof(float) * ((size_t)roi_out->width * roi_out->height));

  DT_OMP_FOR()
  for(int j = 0; j < roi_out->height; j++)
  {
    float *in = (float *)ivoid + (size_t)j * roi_out->width * ch;
    float *lm = luminance + (size_t)j * roi_out->width;
    for(int i = 0; i < roi_out->width; i++)
    {
      const float pmax = CLIP(max3f(in)); // Max value in RGB set
      const float pmin = CLIP(min3f(in)); // Min value in RGB set
      *lm = (pmax + pmin) / 2.f;    // Pixel luminosity
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
  DT_OMP_FOR()
  for(int j = 0; j < roi_out->height; j++)
  {
    const int yMin = fmax(0, j - rad);
    const int yMax = fmin(roi_in->height, j + rad + 1);
    const int h = yMax - yMin;

    const int xMin0 = fmax(0, 0 - rad);
    const int xMax0 = fmin(roi_in->width - 1, rad);

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

      const int xMin = fmax(0, i - rad);
      const int xMax = i + rad + 1;
      const int w = fmin(roi_in->width, xMax) - xMin;
      const int n = h * w;

      const int limit = (int)(slope * n / BINS + 0.5f);

      /* remove left behind values from histogram */
      if(xMin > 0)
      {
        const int xMin1 = xMin - 1;
        for(int yi = yMin; yi < yMax; ++yi)
          --hist[ROUND_POSISTIVE
                 (luminance[(size_t)yi * roi_in->width + xMin1] * (float)BINS)];
      }

      /* add newly included values to histogram */
      if(xMax <= roi_in->width)
      {
        const int xMax1 = xMax - 1;
        for(int yi = yMin; yi < yMax; ++yi)
          ++hist[ROUND_POSISTIVE
                 (luminance[(size_t)yi * roi_in->width + xMax1] * (float)BINS)];
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
          const int d = clippedhist[b] - limit;
          if(d > 0)
          {
            ce += d;
            clippedhist[b] = limit;
          }
        }

        const int d = (ce / (float)(BINS + 1));
        const int m = ce % (BINS + 1);
        for(int b = 0; b <= BINS; b++) clippedhist[b] += d;

        if(m != 0)
        {
          const int s = BINS / (float)m;
          for(int b = 0; b <= BINS; b += s) ++clippedhist[b];
        }
      } while(ce != ceb);

      /* build cdf of clipped histogram */
      unsigned int hMin = BINS;
      for(int b = 0; b < hMin; b++)
        if(clippedhist[b] != 0) hMin = b;

      int cdf = 0;
      for(int b = hMin; b <= v; b++)
        cdf += clippedhist[b];

      int cdfMax = cdf;
      for(int b = v + 1; b <= BINS; b++)
        cdfMax += clippedhist[b];

      const int cdfMin = clippedhist[hMin];

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

static void radius_callback(GtkWidget *slider,
                            dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_rlce_params_t *p = self->params;
  p->radius = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void slope_callback(GtkWidget *slider,
                           dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_rlce_params_t *p = self->params;
  p->slope = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}



void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rlce_params_t *p = (dt_iop_rlce_params_t *)p1;
  dt_iop_rlce_data_t *d = piece->data;

  d->radius = p->radius;
  d->slope = p->slope;
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_rlce_data_t));
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_rlce_gui_data_t *g = self->gui_data;
  dt_iop_rlce_params_t *p = self->params;
  dt_bauhaus_slider_set(g->scale1, p->radius);
  dt_bauhaus_slider_set(g->scale2, p->slope);
}

void init(dt_iop_module_t *self)
{
  self->params = calloc(1, sizeof(dt_iop_rlce_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_rlce_params_t));
  self->default_enabled = FALSE;
  self->params_size = sizeof(dt_iop_rlce_params_t);
  self->gui_data = NULL;
  *((dt_iop_rlce_params_t *)self->default_params) = (dt_iop_rlce_params_t){ 64, 1.25 };
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_rlce_gui_data_t *g = IOP_GUI_ALLOC(rlce);
  dt_iop_rlce_params_t *p = self->default_params;

  g->scale1 = dt_bauhaus_slider_new_with_range(NULL, 0.0, 256.0, 0, p->radius, 0);
  g->scale2 = dt_bauhaus_slider_new_with_range(NULL, 1.0, 3.0, 0, p->slope, 2);
  dt_bauhaus_widget_set_label(g->scale1, NULL, _("radius"));
  dt_bauhaus_widget_set_label(g->scale2, NULL, _("amount"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->scale1), _("size of features to preserve"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->scale2), _("strength of the effect"));

  g_signal_connect(G_OBJECT(g->scale1), "value-changed",
                   G_CALLBACK(radius_callback), self);
  g_signal_connect(G_OBJECT(g->scale2), "value-changed",
                   G_CALLBACK(slope_callback), self);

  self->widget = dt_gui_vbox(g->scale1, g->scale2);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
