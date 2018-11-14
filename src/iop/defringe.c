/*
    This file is part of darktable,
    copyright (c) 2013-2014 dennis gnad.

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
#include "common/darktable.h"
#include "common/gaussian.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"
#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(1, dt_iop_defringe_params_t)

typedef enum dt_iop_defringe_mode_t
{
  MODE_GLOBAL_AVERAGE = 0,
  MODE_LOCAL_AVERAGE = 1,
  MODE_STATIC = 2
} dt_iop_defringe_mode_t;

typedef struct dt_iop_defringe_params_t
{
  float radius;
  float thresh;
  dt_iop_defringe_mode_t op_mode;
} dt_iop_defringe_params_t;

typedef dt_iop_defringe_params_t dt_iop_defringe_data_t;

typedef struct dt_iop_defringe_gui_data_t
{
  GtkBox *vbox;
  GtkWidget *mode_select;
  GtkWidget *radius_scale;
  GtkWidget *thresh_scale;
} dt_iop_defringe_gui_data_t;

// would be nice to be able to precompute this only once for an image
// typedef struct dt_iop_defringe_global_data_t
//{
//  float avg_edge_chroma;
//}
// dt_iop_defringe_global_data_t;


const char *name()
{
  return _("defringe");
}

int groups()
{
  return dt_iop_get_group("defringe", IOP_GROUP_CORRECT);
}

int flags()
{
  // a second instance might help to reduce artifacts when thick fringe needs to be removed
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

// try without clipping for now, usually it should be fine
//#define CLIP(x,y,z)  if (x < y) x = y; if (x > z) x = z;

// Verify before actually using this
/*
void tiling_callback  (dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_in,
const dt_iop_roi_t *roi_out, dt_develop_tiling_t *tiling)
{
  dt_iop_defringe_data_t *p = (dt_iop_defringe_data_t *)piece->data;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int channels = piece->colors;
  const size_t basebuffer = width*height*channels*sizeof(float);

  tiling->factor = 2.0f + (float)dt_gaussian_memory_use(width, height, channels)/basebuffer;
  tiling->maxbuf = fmax(1.0f, (float)dt_gaussian_singlebuffer_size(width, height, channels)/basebuffer);
  tiling->overhead = 0;
  tiling->overlap = p->window;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}
*/

// fibonacci lattice to select surrounding pixels for different cases
static const float fib[] = { 0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233 };
// 0,1,2,3,4,5,6, 7, 8, 9,10,11, 12, 13

static inline void fib_latt(int *const x, int *const y, float radius, int step, int idx)
{
  // idx < 1 because division by zero is also a problem in the following line
  if(idx >= sizeof(fib) / sizeof(float) - 1 || idx < 1)
  {
    *x = 0;
    *y = 0;
    fprintf(stderr, "Fibonacci lattice index wrong/out of bounds in: defringe module\n");
    return;
  }
  float px = step / fib[idx], py = step * (fib[idx + 1] / fib[idx]);
  py -= (int)py;
  float dx = px * radius, dy = py * radius;
  *x = round(dx - radius / 2.0);
  *y = round(dy - radius / 2.0);
}

#define MAGIC_THRESHOLD_COEFF 33.0

// the basis of how the following algorithm works comes from rawtherapee (http://rawtherapee.com/)
// defringe -- thanks to Emil Martinec <ejmartin@uchicago.edu> for that
// quite some modifications were done though:
// 1. use a fibonacci lattice instead of full window, to speed things up
// 2. option for local averaging or static (RT used the global/region one)
// 3. additional condition to reduce sharp edged artifacts, by blurring pixels near pixels over threshold,
// this really helps improving the filter with thick fringes
// -----------------------------------------------------------------------------------------
// in the following you will also see some more "magic numbers",
// most are chosen arbitrarily and/or by experiment/trial+error ... I am sorry ;-)
// and having everything user-defineable would be just too much
// -----------------------------------------------------------------------------------------
void process(struct dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, const void *const i,
             void *const o, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_defringe_data_t *d = (dt_iop_defringe_data_t *)piece->data;
  assert(dt_iop_module_colorspace(module) == iop_cs_Lab);

  const int order = 1; // 0,1,2
  const float sigma = fmax(0.1f, fabs(d->radius)) * roi_in->scale / piece->iscale;
  const float Labmax[] = { 100.0f, 128.0f, 128.0f, 1.0f };
  const float Labmin[] = { 0.0f, -128.0f, -128.0f, 0.0f };
  const int ch = piece->colors;

  const int radius = ceil(2.0 * ceilf(sigma));

  // save the fibonacci lattices in them later
  int *xy_avg = NULL;
  int *xy_artifact = NULL;
  int *xy_small = NULL;

  if(roi_out->width < 2 * radius + 1 || roi_out->height < 2 * radius + 1) goto ERROR_EXIT;

  float avg_edge_chroma = 0.0;

  float *const in = (float *const)i;
  float *const out = (float *const)o;
  int width = roi_in->width;
  int height = roi_in->height;

  dt_gaussian_t *gauss = NULL;
  gauss = dt_gaussian_init(width, height, 4, Labmax, Labmin, sigma, order);
  if(!gauss)
  {
    fprintf(stderr, "Error allocating memory for gaussian blur in: defringe module\n");
    goto ERROR_EXIT;
  }
  dt_gaussian_blur_4c(gauss, in, out);
  dt_gaussian_free(gauss);

  int samples_wish = radius * radius;
  int sampleidx_avg;
  // select samples by fibonacci number
  if(samples_wish > 89)
  {
    sampleidx_avg = 12; // 144 samples
  }
  else if(samples_wish > 55)
  {
    sampleidx_avg = 11; // 89 samples
  }
  else if(samples_wish > 34)
  {
    sampleidx_avg = 10; // ..you get the idea
  }
  else if(samples_wish > 21)
  {
    sampleidx_avg = 9;
  }
  else if(samples_wish > 13)
  {
    sampleidx_avg = 8;
  }
  else
  { // don't use less than 13 samples
    sampleidx_avg = 7;
  }
  const int sampleidx_small = sampleidx_avg - 1;

  const int small_radius = MAX(radius, 3);
  const int avg_radius = 24 + radius * 4;

  const int samples_small = fib[sampleidx_small];
  const int samples_avg = fib[sampleidx_avg];

  // Pre-Compute Fibonacci Lattices

  // precompute all required fibonacci lattices:
  if((xy_avg = malloc((size_t)2 * sizeof(int) * samples_avg)))
  {
    int *tmp = xy_avg;
    for(int u = 0; u < samples_avg; u++)
    {
      int dx, dy;
      fib_latt(&dx, &dy, avg_radius, u, sampleidx_avg);
      *tmp++ = dx;
      *tmp++ = dy;
    }
  }
  else
  {
    fprintf(stderr, "Error allocating memory for fibonacci lattice in: defringe module\n");
    goto ERROR_EXIT;
  }

  if((xy_small = malloc((size_t)2 * sizeof(int) * samples_small)))
  {
    int *tmp = xy_small;
    for(int u = 0; u < samples_small; u++)
    {
      int dx, dy;
      fib_latt(&dx, &dy, small_radius, u, sampleidx_small);
      *tmp++ = dx;
      *tmp++ = dy;
    }
  }
  else
  {
    fprintf(stderr, "Error allocating memory for fibonacci lattice in: defringe module\n");
    goto ERROR_EXIT;
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(width, height,                                                 \
                                              d) reduction(+ : avg_edge_chroma) schedule(static)
#endif
  for(int v = 0; v < height; v++)
  {
    for(int t = 0; t < width; t++)
    {
      // edge-detect on color channels
      // method: difference of original to gaussian blurred image:
      float a = in[(size_t)v * width * ch + t * ch + 1] - out[(size_t)v * width * ch + t * ch + 1];
      float b = in[(size_t)v * width * ch + t * ch + 2] - out[(size_t)v * width * ch + t * ch + 2];

      float edge = (a * a + b * b); // range up to 2*(256)^2 -> approx. 0 to 131072

      // save local edge chroma in out[.. +3] , this is later compared with threshold
      out[(size_t)v * width * ch + t * ch + 3] = edge;
      // the average chroma of the edge-layer in the roi
      if(MODE_GLOBAL_AVERAGE == d->op_mode) avg_edge_chroma += edge;
    }
  }

  float thresh;
  if(MODE_GLOBAL_AVERAGE == d->op_mode)
  {
    avg_edge_chroma = avg_edge_chroma / (width * height) + 10.0 * FLT_EPSILON;
    thresh = fmax(0.1f, 4.0 * d->thresh * avg_edge_chroma / MAGIC_THRESHOLD_COEFF);
  }
  else
  {
    // this fixed value will later be changed when doing local averaging, or kept as-is in "static" mode
    avg_edge_chroma = MAGIC_THRESHOLD_COEFF;
    thresh = fmax(0.1f, d->thresh);
  }

#ifdef _OPENMP
// dynamically/guided scheduled due to possible uneven edge-chroma distribution (thanks to rawtherapee code
// for this hint!)
#pragma omp parallel for default(none) shared(width, height, d, xy_small, xy_avg, xy_artifact)               \
    firstprivate(thresh, avg_edge_chroma) schedule(guided, 32)
#endif
  for(int v = 0; v < height; v++)
  {
    for(int t = 0; t < width; t++)
    {
      float local_thresh = thresh;
      // think of compiler setting "-funswitch-loops" to maybe improve these things:
      if(MODE_LOCAL_AVERAGE == d->op_mode && out[(size_t)v * width * ch + t * ch + 3] > thresh)
      {
        float local_avg = 0.0;
        // use some and not all values from the neighbourhood to speed things up:
        const int *tmp = xy_avg;
        for(int u = 0; u < samples_avg; u++)
        {
          int dx = *tmp++;
          int dy = *tmp++;
          int x = MAX(0, MIN(width - 1, t + dx));
          int y = MAX(0, MIN(height - 1, v + dy));
          local_avg += out[(size_t)y * width * ch + x * ch + 3];
        }
        avg_edge_chroma = fmax(0.01f, (float)local_avg / samples_avg);
        local_thresh = fmax(0.1f, 4.0 * d->thresh * avg_edge_chroma / MAGIC_THRESHOLD_COEFF);
      }

      if(out[(size_t)v * width * ch + t * ch + 3] > local_thresh
         // reduces artifacts ("region growing by 1 pixel"):
         || out[(size_t)MAX(0, (v - 1)) * width * ch + MAX(0, (t - 1)) * ch + 3] > local_thresh
         || out[(size_t)MAX(0, (v - 1)) * width * ch + t * ch + 3] > local_thresh
         || out[(size_t)MAX(0, (v - 1)) * width * ch + MIN(width - 1, (t + 1)) * ch + 3] > local_thresh
         || out[(size_t)v * width * ch + MAX(0, (t - 1)) * ch + 3] > local_thresh
         || out[(size_t)v * width * ch + MIN(width - 1, (t + 1)) * ch + 3] > local_thresh
         || out[(size_t)MIN(height - 1, (v + 1)) * width * ch + MAX(0, (t - 1)) * ch + 3] > local_thresh
         || out[(size_t)MIN(height - 1, (v + 1)) * width * ch + t * ch + 3] > local_thresh
         || out[(size_t)MIN(height - 1, (v + 1)) * width * ch + MIN(width - 1, (t + 1)) * ch + 3]
            > local_thresh)
      {
        float atot = 0, btot = 0;
        float norm = 0;
        float weight;
        // it seems better to use only some pixels from a larger window instead of all pixels from a smaller
        // window
        // we use a fibonacci lattice for that, samples amount need to be a fibonacci number, this can then be
        // scaled to
        // a certain radius

        // use some neighbourhood pixels for lowest chroma average
        const int *tmp = xy_small;
        for(int u = 0; u < samples_small; u++)
        {
          int dx = *tmp++;
          int dy = *tmp++;
          int x = MAX(0, MIN(width - 1, t + dx));
          int y = MAX(0, MIN(height - 1, v + dy));
          // inverse chroma weighted average of neighbouring pixels inside window
          // also taking average edge chromaticity into account (either global or local average)
          weight = 1.0 / (out[(size_t)y * width * ch + x * ch + 3] + avg_edge_chroma);
          atot += weight * in[(size_t)y * width * ch + x * ch + 1];
          btot += weight * in[(size_t)y * width * ch + x * ch + 2];
          norm += weight;
        }
        // here we could try using a "balance" between original and changed value, this could be used to
        // reduce artifcats
        // but on first tries, results weren't very convincing, and there are blend settings available anyway
        // in dt
        // float balance = (out[v*width*ch +t*ch +3]-thresh)/out[v*width*ch +t*ch +3];
        double a = (atot / norm); // *balance + in[v*width*ch + t*ch +1]*(1.0-balance);
        double b = (btot / norm); // *balance + in[v*width*ch + t*ch +2]*(1.0-balance);
        // if (a < -128.0 || a > 127.0) CLIP(a,-128.0,127.0);
        // if (b < -128.0 || b > 127.0) CLIP(b,-128.0,127.0);
        out[(size_t)v * width * ch + t * ch + 1] = a;
        out[(size_t)v * width * ch + t * ch + 2] = b;
      }
      else
      {
        out[(size_t)v * width * ch + t * ch + 1] = in[(size_t)v * width * ch + t * ch + 1];
        out[(size_t)v * width * ch + t * ch + 2] = in[(size_t)v * width * ch + t * ch + 2];
      }
      out[(size_t)v * width * ch + t * ch] = in[(size_t)v * width * ch + t * ch];
    }
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(i, o, roi_out->width, roi_out->height);

  goto FINISH_PROCESS;

ERROR_EXIT:
  memcpy(o, i, (size_t)sizeof(float) * ch * roi_out->width * roi_out->height);

FINISH_PROCESS:
  free(xy_artifact);
  free(xy_small);
  free(xy_avg);
}

void reload_defaults(dt_iop_module_t *module)
{
  module->default_enabled = 0;
  ((dt_iop_defringe_params_t *)module->default_params)->radius = 4.0;
  ((dt_iop_defringe_params_t *)module->default_params)->thresh = 20.0;
  ((dt_iop_defringe_params_t *)module->default_params)->op_mode = MODE_GLOBAL_AVERAGE;
  memcpy(module->params, module->default_params, sizeof(dt_iop_defringe_params_t));
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_defringe_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_defringe_params_t));
  module->priority = 414; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_defringe_params_t);
  module->gui_data = NULL;
  module->data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

static void radius_slider_callback(GtkWidget *w, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  dt_iop_defringe_params_t *p = (dt_iop_defringe_params_t *)module->params;
  p->radius = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, module, TRUE);
}

static void thresh_slider_callback(GtkWidget *w, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  dt_iop_defringe_params_t *p = (dt_iop_defringe_params_t *)module->params;
  p->thresh = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, module, TRUE);
}

static void mode_callback(GtkWidget *w, dt_iop_module_t *module)
{
  dt_iop_defringe_params_t *p = (dt_iop_defringe_params_t *)module->params;
  p->op_mode = dt_bauhaus_combobox_get(w);
  dt_dev_add_history_item(darktable.develop, module, TRUE);
}

void gui_init(dt_iop_module_t *module)
{
  module->gui_data = malloc(sizeof(dt_iop_defringe_gui_data_t));
  dt_iop_defringe_gui_data_t *g = (dt_iop_defringe_gui_data_t *)module->gui_data;
  dt_iop_defringe_params_t *p = (dt_iop_defringe_params_t *)module->params;

  module->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(module->widget, dt_get_help_url(module->op));

  /* mode selection */
  g->mode_select = dt_bauhaus_combobox_new(module);
  gtk_box_pack_start(GTK_BOX(module->widget), g->mode_select, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->mode_select, NULL, _("operation mode"));
  dt_bauhaus_combobox_add(g->mode_select, _("global average (fast)"));   // 0
  dt_bauhaus_combobox_add(g->mode_select, _("local average (slow)"));    // 1
  dt_bauhaus_combobox_add(g->mode_select, _("static threshold (fast)")); // 2
  gtk_widget_set_tooltip_text(g->mode_select,
      _("method for color protection:\n - global average: fast, might show slightly wrong previews in high "
        "magnification; might sometimes protect saturation too much or too low in comparison to local "
        "average\n - local average: slower, might protect saturation better than global average by using "
        "near pixels as color reference, so it can still allow for more desaturation where required\n - "
        "static: fast, only uses the threshold as a static limit"));
  g_signal_connect(G_OBJECT(g->mode_select), "value-changed", G_CALLBACK(mode_callback), module);

  /* radius and threshold sliders */
  g->radius_scale = dt_bauhaus_slider_new_with_range(module, 0.5, 20.0, 0.1, p->radius, 1);
  dt_bauhaus_widget_set_label(g->radius_scale, NULL, _("edge detection radius"));

  g->thresh_scale = dt_bauhaus_slider_new_with_range(module, 0.5, 128.0, 0.1, p->thresh, 1);
  dt_bauhaus_widget_set_label(g->thresh_scale, NULL, _("threshold"));

  gtk_box_pack_start(GTK_BOX(module->widget), GTK_WIDGET(g->radius_scale), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(module->widget), GTK_WIDGET(g->thresh_scale), TRUE, TRUE, 0);

  gtk_widget_set_tooltip_text(g->radius_scale, _("radius for detecting fringe"));
  gtk_widget_set_tooltip_text(g->thresh_scale, _("threshold for defringe, higher values mean less defringing"));

  g_signal_connect(G_OBJECT(g->radius_scale), "value-changed", G_CALLBACK(radius_slider_callback), module);
  g_signal_connect(G_OBJECT(g->thresh_scale), "value-changed", G_CALLBACK(thresh_slider_callback), module);
}

void gui_update(dt_iop_module_t *module)
{
  dt_iop_defringe_gui_data_t *g = (dt_iop_defringe_gui_data_t *)module->gui_data;
  dt_iop_defringe_params_t *p = (dt_iop_defringe_params_t *)module->params;
  dt_bauhaus_combobox_set(g->mode_select, p->op_mode);
  dt_bauhaus_slider_set(g->radius_scale, p->radius);
  dt_bauhaus_slider_set(g->thresh_scale, p->thresh);
}

void gui_cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
