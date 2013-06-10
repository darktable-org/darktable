/*
    This file is part of darktable,
    copyright (c) 2013 dennis gnad.

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
#include <assert.h>
#include <string.h>
#include <math.h>
#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "develop/imageop.h"
#include "dtgtk/slider.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <stdlib.h>
#include "common/gaussian.h"

DT_MODULE(1)

#define MODE_GLOBAL_AVERAGE 0
#define MODE_LOCAL_AVERAGE 1
#define MODE_STATIC 2

typedef struct dt_iop_defringe_params_t
{
  float radius;
  float thresh;
  uint32_t op_mode;
  float a_bias;
  float b_bias;
}
dt_iop_defringe_params_t;

typedef dt_iop_defringe_params_t dt_iop_defringe_data_t;

typedef struct dt_iop_defringe_gui_data_t
{
  GtkVBox *vbox;
  GtkWidget *mode_select;
  GtkWidget *radius_scale;
  GtkWidget *thresh_scale;
  GtkWidget *a_bias_scale;
  GtkWidget *b_bias_scale;
}
dt_iop_defringe_gui_data_t;

// would be nice to be able to precompute this only once for an image
//typedef struct dt_iop_defringe_global_data_t
//{
//  float avg_edge_chroma;
//}
//dt_iop_defringe_global_data_t;

const char *name()
{
  return _("defringe");
}

int groups ()
{
  return IOP_GROUP_CORRECT;
}

int flags ()
{
  // a second instance might help to reduce artifacts when thick fringe needs to be removed
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

int
output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return sizeof(float);
}

// Verify before actually using this
/*
void tiling_callback  (dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, dt_develop_tiling_t *tiling)
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
static const float fib[] =
{ 0,1,1,2,3,5,8,13,21,34,55,89,144,233};
//0,1,2,3,4,5,6, 7, 8, 9,10,11, 12, 13

static inline
void fib_latt(int * const x, int * const y, float radius, int step, int idx)
{
  float px = step/fib[idx], py = step*(fib[idx+1]/fib[idx]);
  py -= (int)py;
  float dx = px*radius, dy = py*radius;
  *x = round(dx - radius/2.0);
  *y = round(dy - radius/2.0);
}

#define MAGIC_THRESHOLD_COEFF 33.0

// the basis of how the following algorithm works comes from rawtherapee's (http://rawtherapee.com/)
// defringe -- thanks to Emil Martinec <ejmartin@uchicago.edu> for that
// quite some modifications were done though:
// 1. use a fibonacci lattice instead of full window, to speed thigns up
// 2. option for local averaging or static (RT used the global/region one)
// 3. color shift/bias options
// 4. additional condition to reduce sharp edged artifacts, by blurring pixels near to pixels over threshold
// -----------------------------------------------------------------------------------------
// in the following you will also see some "magic numbers",
// most are chosen arbitrarily and/or by experiment/trial+error ... I am sorry ;-)
// having everything user-defineable would be just too much
// -----------------------------------------------------------------------------------------
void process (struct dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_defringe_data_t *p = (dt_iop_defringe_data_t *)piece->data;
  assert(dt_iop_module_colorspace(module) == iop_cs_Lab);

  const int order = 1; // 0,1,2
  const float sigma = p->radius * roi_in->scale / piece->iscale;
  const float Labmax[] = { 100.0f, 128.0f, 128.0f, 1.0f };
  const float Labmin[] = { 0.0f, -128.0f, -128.0f, 0.0f };
  const int ch = piece->colors;

  const int radius = ceil(2.0*ceilf(sigma));

  float avg_edge_chroma = 0.0;

  float * const in = (float*const)i;
  float * const out = (float*const)o;
  int width = roi_in->width;
  int height = roi_in->height;

  dt_gaussian_t * gauss = dt_gaussian_init(width, height, ch, Labmax, Labmin, sigma, order);
  if (!gauss) return;
  dt_gaussian_blur(gauss, in, out);
  dt_gaussian_free(gauss);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(width,height,p) reduction(+:avg_edge_chroma) schedule(static)
#endif
  for (int v=0; v<height; v++)
  {
    for (int t=0; t<width; t++)
    {
      // edge-detect on color channels
      // method: difference of original to gaussian blurred image:
      float a = in[v*width*ch + t*ch +1] - out[v*width*ch + t*ch +1];
      float b = in[v*width*ch + t*ch +2] - out[v*width*ch + t*ch +2];

      // color biasing
      if (a > 0.0)
        a = a*(0.1+p->a_bias); // magenta bias
      else
        a = a*(0.1+(1-p->a_bias)); // green bias

      if (b > 0.0)
        b = b*(0.1+p->b_bias); // yellow bias
      else
        b = b*(0.1+(1-p->b_bias)); // blue bias

      float edge = (a*a+b*b); //range up to 2 * (1.1*256)^2 -> approx. 0 to 158598
      float saturation = fabs(in[v*width*ch + t*ch +1]) + fabs(in[v*width*ch + t*ch +2]);
      // save local edge chroma in out[.. +3] , this is later compared with threshold
      // partially dependent on luminosity / saturation
      out[v*width*ch + t*ch +3] = edge * 0.6 + edge * 0.4 * saturation/255.0 * in[v*width*ch + t*ch]/255.0;
      // the average chroma of the edge-layer in the roi
      if (MODE_GLOBAL_AVERAGE == p->op_mode) avg_edge_chroma += edge;
    }
  }

  float thresh;
  if (MODE_GLOBAL_AVERAGE == p->op_mode)
  {
    avg_edge_chroma = avg_edge_chroma / (width * height);
    thresh = 8.0 * p->thresh * avg_edge_chroma / MAGIC_THRESHOLD_COEFF;
  }
  else
  {
    // this fixed value will later be changed when doing local averaging, or kept as-is in "static" mode
    avg_edge_chroma = MAGIC_THRESHOLD_COEFF;
    thresh = 6.0 * p->thresh;
  }

  // precompute stuff
  int samples_wish = radius*radius*0.8;
  int sampleidx;
  // select samples by fibonacci number
  if (samples_wish > 89) {
    sampleidx = 12; // 144 samples
  } else if (samples_wish > 55) {
    sampleidx = 11; // 89 samples
  } else if (samples_wish > 34) {
    sampleidx = 10; // ..you get the idea
  } else if (samples_wish > 21) {
    sampleidx = 9;
  } else if (samples_wish > 13) {
    sampleidx = 8;
  } else { // don't use less than 13 samples
    sampleidx = 7;
  }
  const int sampleidx_small = sampleidx-1;
  // larger area when using local averaging
  const int radius_avg = ceil(radius*10);
  // smaller area for artifact filter
  const int radius_artifact_filter = radius/2;

#ifdef _OPENMP
// dynamically scheduled due to possible uneven edge-chroma distribution (thanks to rawtherapee code for this hint!)
#pragma omp parallel for default(none) shared(width,height,p,sampleidx) firstprivate(thresh,avg_edge_chroma) schedule(dynamic,32)
#endif
  for (int v=0; v<height; v++)
  {
    for (int t=0; t<width; t++)
    {
      if (out[v*width*ch +t*ch +3] > thresh)
      {
        float atot=0, btot=0;
        float norm=0;
        float weight;
        // it seems better to use only some pixels from a larger window instead of all pixels from a smaller window
        // we use a fibonacci lattice for that, samples amount need to be a fibonacci number, this can then be scaled to
        // a certain radius

        // "-funswitch-loops" should unswitch all such things
        if (MODE_LOCAL_AVERAGE == p->op_mode)
        {
          float local_avg = 0.0;
          // use some and not all values from the neigbourhood to speed things up
          for (int u=0; u < fib[sampleidx]; u++)
          {
            int dx,dy;
            fib_latt(&dx,&dy,radius_avg,u,sampleidx);
            int x = MAX(0,MIN(height,v+dx));
            int y = MAX(0,MIN(width,t+dy));
            local_avg += out[x*width*ch + y*ch +3];
          }
          local_avg /= (float)fib[sampleidx]*2.0;
          avg_edge_chroma = local_avg;
        }
        // use some neighbourhood pixels for lowest chroma average
        for (int u=0; u < fib[sampleidx_small]; u++)
        {
          int dx,dy;
          fib_latt(&dx,&dy,radius,u,sampleidx_small);
          int x = MAX(0,MIN(height,v+dx));
          int y = MAX(0,MIN(width,t+dy));
          // inverse chroma weighted average of neigbouring pixels inside window
          // also taking average edge chromaticity into account (either global or local average)
          weight = 1.0/(out[x*width*ch + y*ch +3] + avg_edge_chroma);
          atot += weight * in[x*width*ch + y*ch +1];
          btot += weight * in[x*width*ch + y*ch +2];
          norm += weight;
        }
        // here we could try using a "balance" between original and changed value, this could be used to reduce artifcats
        // but on first tries, results weren't very convincing
        // float balance = (out[v*width*ch +t*ch +3]-thresh)/out[v*width*ch +t*ch +3];
        out[v*width*ch + t*ch +1] = (atot/norm); // *balance + in[v*width*ch + t*ch +1]*(1.0-balance);
        out[v*width*ch + t*ch +2] = (btot/norm); // *balance + in[v*width*ch + t*ch +2]*(1.0-balance);*/
      }
      // "artifact reduction filter": iterate also over neighbours of pixel over threshold
      // reducing artifacts could still be better, especially for fringe with a thickness of more than 1..2 pixels
      else if ( out[MAX(0,(v-1))*width*ch +MAX(0,(t-1))*ch +3] > thresh
                || out[MAX(0,(v-1))*width*ch +t*ch +3] > thresh
                || out[MAX(0,(v-1))*width*ch +MIN(width,(t+1))*ch +3] > thresh
                || out[v*width*ch +MAX(0,(t-1))*ch +3] > thresh
                || out[v*width*ch +MIN(width,(t+1))*ch +3] > thresh
                || out[MIN(height,(v+1))*width*ch +MAX(0,(t-1))*ch +3] > thresh
                || out[MIN(height,(v+1))*width*ch +t*ch +3] > thresh
                || out[MIN(height,(v+1))*width*ch +MIN(width,(t+1))*ch +3] > thresh )
      {
        float atot=0, btot=0;
        float norm=0;
        float weight;
        // dup'ed code from above:
        for (int u=0; u < fib[sampleidx_small]; u++)
        {
          int dx,dy;
          fib_latt(&dx,&dy,radius_artifact_filter,u,sampleidx_small);
          int x = MAX(0,MIN(height,v+dx));
          int y = MAX(0,MIN(width,t+dy));
          // inverse chroma weighted average of neigbouring pixels inside window
          // also taking average edge chromaticity into account (either global or local average)
          weight = 1.0/(out[x*width*ch + y*ch +3] + avg_edge_chroma);
          atot += weight * in[x*width*ch + y*ch +1];
          btot += weight * in[x*width*ch + y*ch +2];
          norm += weight;
        }
        out[v*width*ch + t*ch +1] = (atot/norm);
        out[v*width*ch + t*ch +2] = (btot/norm);
      }
      else
      {
        out[v*width*ch + t*ch +1] = in[v*width*ch + t*ch +1];
        out[v*width*ch + t*ch +2] = in[v*width*ch + t*ch +2];
      }
    }
  }

  // yes this needs to be done in an extra loop (at least for out[.. +3] that was used for edge values)
  // we might want to use this in a kind of "deferred" way in the upper loop to have a higher probability
  // of having this still in cache - but probably not worth it..
  for (int v=0; v<height; v++)
  {
    for (int t=0; t<width; t++)
    {
      out[v*width*ch + t*ch   ] = in[v*width*ch + t*ch   ];
      out[v*width*ch + t*ch +3] = in[v*width*ch + t*ch +3];
    }
  }
}

void reload_defaults(dt_iop_module_t *module)
{
  module->default_enabled = 0;
  ((dt_iop_defringe_params_t *)module->default_params)->radius = 4.0;
  ((dt_iop_defringe_params_t *)module->default_params)->thresh = 20;
  ((dt_iop_defringe_params_t *)module->default_params)->op_mode = MODE_GLOBAL_AVERAGE;
  ((dt_iop_defringe_params_t *)module->default_params)->a_bias = +0.2; // more reddish -> "purple fringe"
  ((dt_iop_defringe_params_t *)module->default_params)->b_bias = -0.3; // more blueish -> purple
  memcpy(module->params, module->default_params, sizeof(dt_iop_defringe_params_t));
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_defringe_params_t));
  module->default_params = malloc(sizeof(dt_iop_defringe_params_t));
  module->request_histogram = 1;
  module->priority = 385; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_defringe_params_t);
  module->gui_data = NULL;
  module->data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
  free(module->default_params);
  module->default_params = NULL;
}

static void
radius_slider_callback (GtkWidget *w, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  dt_iop_defringe_params_t *p = (dt_iop_defringe_params_t *)module->params;
  p->radius = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, module, TRUE);
}

static void
thresh_slider_callback (GtkWidget *w, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  dt_iop_defringe_params_t *p = (dt_iop_defringe_params_t *)module->params;
  p->thresh = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, module, TRUE);
}

static void
a_bias_slider_callback (GtkWidget *w, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  dt_iop_defringe_params_t *p = (dt_iop_defringe_params_t *)module->params;
  p->a_bias = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, module, TRUE);
}

static void
b_bias_slider_callback (GtkWidget *w, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  dt_iop_defringe_params_t *p = (dt_iop_defringe_params_t *)module->params;
  p->b_bias = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, module, TRUE);
}

static void
mode_callback (GtkWidget *w, dt_iop_module_t *module)
{
  dt_iop_defringe_params_t *p = (dt_iop_defringe_params_t *)module->params;
  p->op_mode = dt_bauhaus_combobox_get(w);
  dt_dev_add_history_item(darktable.develop, module, TRUE);
}

void gui_init (dt_iop_module_t *module)
{
  module->gui_data = malloc(sizeof(dt_iop_defringe_gui_data_t));
  dt_iop_defringe_gui_data_t *g = (dt_iop_defringe_gui_data_t *)module->gui_data;
  dt_iop_defringe_params_t *p = (dt_iop_defringe_params_t *)module->params;

  module->widget = gtk_vbox_new(FALSE, DT_BAUHAUS_SPACE);

  /* mode selection */
  g->mode_select = dt_bauhaus_combobox_new(module);
  gtk_box_pack_start(GTK_BOX(module->widget), g->mode_select, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->mode_select, NULL, _("operation mode"));
  dt_bauhaus_combobox_add(g->mode_select, _("region average (fast)")); // 0
  dt_bauhaus_combobox_add(g->mode_select, _("local average (slow)")); // 1
  dt_bauhaus_combobox_add(g->mode_select, _("static")); // 2
  g_object_set (GTK_OBJECT(g->mode_select), "tooltip-text", _("method for chroma protection:\n - region average: fast, might show slightly wrong previews in high magnification; might sometimes protect chroma too much or too low in comparison to local average\n - local average: slower, might protect chroma better than global average while still allowing for more desaturation where required\n - static: fast, only uses the threshold as a static limit"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->mode_select), "value-changed", G_CALLBACK (mode_callback), module);

  /* radius and threshold sliders */
  g->radius_scale = dt_bauhaus_slider_new_with_range(module, 0.5, 20.0, 0.1, p->radius, 1);
  dt_bauhaus_widget_set_label(g->radius_scale, NULL, _("radius / strength"));

  g->thresh_scale = dt_bauhaus_slider_new_with_range(module, 1.0, 128.0, 0.1, p->thresh, 1);
  dt_bauhaus_widget_set_label(g->thresh_scale, NULL, _("threshold"));

  gtk_box_pack_start(GTK_BOX(module->widget), GTK_WIDGET(g->radius_scale), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(module->widget), GTK_WIDGET(g->thresh_scale), TRUE, TRUE, 0);

  g_object_set(G_OBJECT(g->radius_scale), "tooltip-text",
               _("radius for fringe detection"), (char *)NULL);
  g_object_set(G_OBJECT(g->thresh_scale), "tooltip-text",
               _("threshold for defringe, higher values mean less defringing"), (char *)NULL);

  g_signal_connect(G_OBJECT(g->radius_scale), "value-changed",
                   G_CALLBACK(radius_slider_callback), module);
  g_signal_connect(G_OBJECT(g->thresh_scale), "value-changed",
                   G_CALLBACK(thresh_slider_callback), module);
  /* color bias/shift/whatever sliders */
  g->a_bias_scale = dt_bauhaus_slider_new_with_range(module, -1.0, 1.0, 0.01, p->a_bias, 2);
  dt_bauhaus_widget_set_label(g->a_bias_scale, NULL, _("green / magenta balance"));
  dt_bauhaus_slider_set_stop(g->a_bias_scale, 0.0f, 0.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(g->a_bias_scale, 1.0f, 1.0f, 0.0f, 1.0f);
  g_object_set(G_OBJECT(g->a_bias_scale), "tooltip-text", _("move between stronger green or magenta sensitivity"), (char *)NULL);

  g->b_bias_scale = dt_bauhaus_slider_new_with_range(module, -1.0, 1.0, 0.01, p->b_bias, 2);
  dt_bauhaus_widget_set_label(g->b_bias_scale, NULL, _("blue / yellow balance"));
  dt_bauhaus_slider_set_stop(g->b_bias_scale, 0.0f, 0.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->b_bias_scale, 1.0f, 1.0f, 1.0f, 0.0f);
  g_object_set(G_OBJECT(g->b_bias_scale), "tooltip-text", _("move between stronger blue or yellow sensitivity"), (char *)NULL);

  g_signal_connect(G_OBJECT(g->a_bias_scale), "value-changed",
                   G_CALLBACK(a_bias_slider_callback), module);
  g_signal_connect(G_OBJECT(g->b_bias_scale), "value-changed",
                   G_CALLBACK(b_bias_slider_callback), module);

  gtk_box_pack_start(GTK_BOX(module->widget), GTK_WIDGET(g->a_bias_scale), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(module->widget), GTK_WIDGET(g->b_bias_scale), TRUE, TRUE, 0);
}

void gui_update (dt_iop_module_t *module)
{
  dt_iop_defringe_gui_data_t *g = (dt_iop_defringe_gui_data_t *)module->gui_data;
  dt_iop_defringe_params_t *p = (dt_iop_defringe_params_t *)module->params;
  dt_bauhaus_combobox_set(g->mode_select, p->op_mode);
  dt_bauhaus_slider_set(g->radius_scale, p->radius);
  dt_bauhaus_slider_set(g->thresh_scale, p->thresh);
  dt_bauhaus_slider_set(g->a_bias_scale, p->a_bias);
  dt_bauhaus_slider_set(g->b_bias_scale, p->b_bias);
}

void gui_cleanup (dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
}
