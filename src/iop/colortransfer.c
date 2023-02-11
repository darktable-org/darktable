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
#include "common/colorspaces.h"
#include "common/imagebuf.h"
#include "common/points.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/**
 * color transfer somewhat based on the glorious paper `color transfer between images'
 * by erik reinhard, michael ashikhmin, bruce gooch, and peter shirley, 2001.
 * chosen because it officially cites the playboy.
 *
 * workflow:
 * - open the target image, press acquire button
 * - right click store as preset
 * - open image you want to transfer the color to
 * - right click and apply the preset
 */

DT_MODULE_INTROSPECTION(1, dt_iop_colortransfer_params_t)

#define HISTN (1 << 11)
#define MAXN 5

typedef float float2[2];

typedef enum dt_iop_colortransfer_flag_t
{
  ACQUIRE = 0,
  ACQUIRE2 = 1,
  ACQUIRE3 = 2,
  ACQUIRED = 3,
  APPLY = 4,
  NEUTRAL = 5
} dt_iop_colortransfer_flag_t;

typedef struct dt_iop_colortransfer_params_t
{
  dt_iop_colortransfer_flag_t flag; // $DEFAULT: NEUTRAL
  // hist matching table
  float hist[HISTN];
  // n-means (max 5?) with mean/variance
  float2 mean[MAXN];
  float2 var[MAXN];
  // number of gaussians used.
  int n; // $DEFAULT: 3
} dt_iop_colortransfer_params_t;

typedef struct dt_iop_colortransfer_gui_data_t
{
  int flowback_set;
  dt_iop_colortransfer_params_t flowback;
  GtkWidget *apply_button;
  GtkWidget *acquire_button;
  GtkSpinButton *spinbutton;
  GtkWidget *area;
  cmsHTRANSFORM xform;
} dt_iop_colortransfer_gui_data_t;

typedef struct dt_iop_colortransfer_data_t
{
  // same as params. (need duplicate because database table preset contains params_t)
  dt_iop_colortransfer_flag_t flag;
  float hist[HISTN];
  float2 mean[MAXN];
  float2 var[MAXN];
  int n;
} dt_iop_colortransfer_data_t;


const char *name()
{
  return _("Color transfer");
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_EFFECTS;
}

int flags()
{
  return IOP_FLAGS_DEPRECATED | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_PREVIEW_NON_OPENCL;
}

const char *deprecated_msg()
{
  return _("This module is deprecated. Better use color mapping module instead.");
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

#if 0
void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_iop(self, FALSE, NC_("accel", "Acquire"), 0, 0);
  dt_accel_register_iop(self, FALSE, NC_("accel", "Apply"), 0, 0);
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_colortransfer_gui_data_t *g =
    (dt_iop_colortransfer_gui_data_t*)self->gui_data;

  dt_accel_connect_button_iop(self, "acquire", g->acquire_button);
  dt_accel_connect_button_iop(self, "apply", g->apply_button);
}
#endif

static void capture_histogram(const float *col, const dt_iop_roi_t *roi, int *hist)
{
  // build separate histogram
  memset(hist, 0, sizeof(int) * HISTN);
  for(int k = 0; k < roi->height; k++)
    for(int i = 0; i < roi->width; i++)
    {
      const int bin = CLAMP(HISTN * col[3 * (k * roi->width + i) + 0] / 100.0, 0, HISTN - 1);
      hist[bin]++;
    }

  // accumulated start distribution of G1 G2
  for(int k = 1; k < HISTN; k++) hist[k] += hist[k - 1];
  for(int k = 0; k < HISTN; k++)
    hist[k] = (int)CLAMP(hist[k] * (HISTN / (float)hist[HISTN - 1]), 0, HISTN - 1);
  // for(int i=0;i<100;i++) printf("#[%d] %d \n", i, hist[(int)CLAMP(HISTN*i/100.0, 0, HISTN-1)]);
}

static void invert_histogram(const int *hist, float *inv_hist)
{
// invert non-normalised accumulated hist
#if 0
  int last = 0;
  for(int i=0; i<HISTN; i++) for(int k=last; k<HISTN; k++)
      if(hist[k] >= i)
      {
        last = k;
        inv_hist[i] = 100.0*k/(float)HISTN;
        break;
      }
#else
  int last = 31;
  for(int i = 0; i <= last; i++) inv_hist[i] = 100.0 * i / (float)HISTN;
  for(int i = last + 1; i < HISTN; i++)
    for(int k = last; k < HISTN; k++)
      if(hist[k] >= i)
      {
        last = k;
        inv_hist[i] = 100.0 * k / (float)HISTN;
        break;
      }
#endif

  // printf("inv histogram debug:\n");
  // for(int i=0;i<100;i++) printf("%d => %f\n", i, inv_hist[hist[(int)CLAMP(HISTN*i/100.0, 0, HISTN-1)]]);
  // for(int i=0;i<100;i++) printf("[%d] %f => %f\n", i, hist[(int)CLAMP(HISTN*i/100.0, 0,
  // HISTN-1)]/(float)HISTN, inv_hist[(int)CLAMP(HISTN*i/100.0, 0, HISTN-1)]);
}

static void get_cluster_mapping(const int n, float2 *mi, float2 *mo, int *mapio)
{
  for(int ki = 0; ki < n; ki++)
  {
    // for each input cluster
    float mdist = FLT_MAX;
    for(int ko = 0; ko < n; ko++)
    {
      // find the best target cluster (the same could be used more than once)
      const float dist = (mo[ko][0] - mi[ki][0]) * (mo[ko][0] - mi[ki][0])
                         + (mo[ko][1] - mi[ki][1]) * (mo[ko][1] - mi[ki][1]);
      if(dist < mdist)
      {
        mdist = dist;
        mapio[ki] = ko;
      }
    }
  }
}

static void get_clusters(const float *col, const int n, float2 *mean, float *weight)
{
  float Mdist = 0.0f, mdist = FLT_MAX;
  for(int k = 0; k < n; k++)
  {
    const float dist = (col[1] - mean[k][0]) * (col[1] - mean[k][0])
                       + (col[2] - mean[k][1]) * (col[2] - mean[k][1]);
    weight[k] = dist;
    if(dist < mdist) mdist = dist;
    if(dist > Mdist) Mdist = dist;
  }
  if(Mdist - mdist > 0)
    for(int k = 0; k < n; k++) weight[k] = (weight[k] - mdist) / (Mdist - mdist);
  float sum = 0.0f;
  for(int k = 0; k < n; k++) sum += weight[k];
  if(sum > 0)
    for(int k = 0; k < n; k++) weight[k] /= sum;
}

static int get_cluster(const float *col, const int n, float2 *mean)
{
  float mdist = FLT_MAX;
  int cluster = 0;
  for(int k = 0; k < n; k++)
  {
    const float dist = (col[1] - mean[k][0]) * (col[1] - mean[k][0])
                       + (col[2] - mean[k][1]) * (col[2] - mean[k][1]);
    if(dist < mdist)
    {
      mdist = dist;
      cluster = k;
    }
  }
  return cluster;
}

static void kmeans(const float *col, const dt_iop_roi_t *const roi, const int n, float2 *mean_out,
                   float2 *var_out)
{
  // TODO: check params here:
  const int nit = 10;                                 // number of iterations
  const int samples = roi->width * roi->height * 0.2; // samples: only a fraction of the buffer.

  float2 *const mean = malloc(sizeof(float2) * n);
  float2 *const var = malloc(sizeof(float2) * n);
  int *const cnt = malloc(sizeof(int) * n);

  // init n clusters for a, b channels at random
  for(int k = 0; k < n; k++)
  {
    mean_out[k][0] = 20.0f - 40.0f * dt_points_get();
    mean_out[k][1] = 20.0f - 40.0f * dt_points_get();
    var_out[k][0] = var_out[k][1] = 0.0f;
    mean[k][0] = mean[k][1] = var[k][0] = var[k][1] = 0.0f;
  }
  for(int it = 0; it < nit; it++)
  {
    for(int k = 0; k < n; k++) cnt[k] = 0;
// randomly sample col positions inside roi
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(cnt, mean, n, roi, samples, var) \
    shared(col, mean_out) \
    schedule(static)
#endif
    for(int s = 0; s < samples; s++)
    {
      const int j = dt_points_get() * roi->height;
      const int i = dt_points_get() * roi->width;
      // for each sample: determine cluster, update new mean, update var
      for(int k = 0; k < n; k++)
      {
        const float L = col[3 * (roi->width * j + i)];
        const dt_aligned_pixel_t Lab = { L, col[3 * (roi->width * j + i) + 1], col[3 * (roi->width * j + i) + 2] };
        // determine dist to mean_out
        const int c = get_cluster(Lab, n, mean_out);
#ifdef _OPENMP
#pragma omp atomic
#endif
        cnt[c]++;
// update mean, var
#ifdef _OPENMP
#pragma omp atomic
#endif
        var[c][0] += Lab[1] * Lab[1];
#ifdef _OPENMP
#pragma omp atomic
#endif
        var[c][1] += Lab[2] * Lab[2];
#ifdef _OPENMP
#pragma omp atomic
#endif
        mean[c][0] += Lab[1];
#ifdef _OPENMP
#pragma omp atomic
#endif
        mean[c][1] += Lab[2];
      }
    }
    // swap old/new means
    for(int k = 0; k < n; k++)
    {
      if(cnt[k] == 0) continue;
      mean_out[k][0] = mean[k][0] / cnt[k];
      mean_out[k][1] = mean[k][1] / cnt[k];
      var_out[k][0] = var[k][0] / cnt[k] - mean_out[k][0] * mean_out[k][0];
      var_out[k][1] = var[k][1] / cnt[k] - mean_out[k][1] * mean_out[k][1];
      mean[k][0] = mean[k][1] = var[k][0] = var[k][1] = 0.0f;
    }
    // printf("it %d  %d means:\n", it, n);
    // for(int k=0;k<n;k++) printf("%f %f -- var %f %f\n", mean_out[k][0], mean_out[k][1], var_out[k][0],
    // var_out[k][1]);
  }
  free(cnt);
  free(var);
  free(mean);
  for(int k = 0; k < n; k++)
  {
    // we actually want the std deviation.
    var_out[k][0] = sqrtf(var_out[k][0]);
    var_out[k][1] = sqrtf(var_out[k][1]);
  }
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  // FIXME: this returns nan!!
  dt_iop_colortransfer_data_t *data = (dt_iop_colortransfer_data_t *)piece->data;
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

  if(data->flag == ACQUIRE)
  {
    if(piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW)
    {
      // only get stuff from the preview pipe, rest stays untouched.
      int hist[HISTN];
      // get histogram of L
      capture_histogram(in, roi_in, hist);
      // invert histogram of L
      invert_histogram(hist, data->hist);

      // get n clusters
      kmeans(in, roi_in, data->n, data->mean, data->var);

      // notify gui that commit_params should let stuff flow back!
      data->flag = ACQUIRED;
      dt_iop_colortransfer_params_t *p = (dt_iop_colortransfer_params_t *)self->params;
      p->flag = ACQUIRE2;
    }
    dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, ch);
  }
  else if(data->flag == APPLY)
  {
    // apply histogram of L and clustering of (a,b)
    int hist[HISTN];
    capture_histogram(in, roi_in, hist);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(ch, roi_out) \
    shared(data, in, out, hist) \
    schedule(static)
#endif
    for(int k = 0; k < roi_out->height; k++)
    {
      size_t j = (size_t)ch * roi_out->width * k;
      for(int i = 0; i < roi_out->width; i++)
      {
        // L: match histogram
        out[j] = data->hist[hist[(int)CLAMP(HISTN * in[j] / 100.0, 0, HISTN - 1)]];
        out[j] = CLAMP(out[j], 0, 100);
        j += ch;
      }
    }

    // cluster input buffer
    float2 *const mean = malloc(sizeof(float2) * data->n);
    float2 *const var = malloc(sizeof(float2) * data->n);

    kmeans(in, roi_in, data->n, mean, var);

    // get mapping from input clusters to target clusters
    int *const mapio = malloc(sizeof(int) * data->n);

    get_cluster_mapping(data->n, mean, data->mean, mapio);

// for all pixels: find input cluster, transfer to mapped target cluster
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(ch, mapio, mean, roi_out, var) \
    shared(data, in, out) \
    schedule(static)
#endif
    for(int k = 0; k < roi_out->height; k++)
    {
      float weight[MAXN];
      size_t j = (size_t)ch * roi_out->width * k;
      for(int i = 0; i < roi_out->width; i++)
      {
        const float L = in[j];
        const dt_aligned_pixel_t Lab = { L, in[j + 1], in[j + 2] };
// a, b: subtract mean, scale nvar/var, add nmean
#if 0 // single cluster, gives color banding
        const int ki = get_cluster(in + j, data->n, mean);
        out[j+1] = 100.0/out[j] * ((Lab[1] - mean[ki][0])*data->var[mapio[ki]][0]/var[ki][0] + data->mean[mapio[ki]][0]);
        out[j+2] = 100.0/out[j] * ((Lab[2] - mean[ki][1])*data->var[mapio[ki]][1]/var[ki][1] + data->mean[mapio[ki]][1]);
#else // fuzzy weighting
        get_clusters(in + j, data->n, mean, weight);
        out[j + 1] = out[j + 2] = 0.0f;
        for(int c = 0; c < data->n; c++)
        {
          out[j + 1] += weight[c] * ((Lab[1] - mean[c][0]) * data->var[mapio[c]][0] / var[c][0]
                                     + data->mean[mapio[c]][0]);
          out[j + 2] += weight[c] * ((Lab[2] - mean[c][1]) * data->var[mapio[c]][1] / var[c][1]
                                     + data->mean[mapio[c]][1]);
        }
#endif
        out[j + 3] = in[j + 3];
        j += ch;
      }
    }

    free(mapio);
    free(var);
    free(mean);
  }
  else
  {
    dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, ch);
  }
}

#if 0
static void
spinbutton_changed (GtkSpinButton *button, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colortransfer_params_t *p = (dt_iop_colortransfer_params_t *)self->params;
//  dt_iop_colortransfer_gui_data_t *g = (dt_iop_colortransfer_gui_data_t *)self->gui_data;
  p->n = gtk_spin_button_get_value(button);
  memset(p->hist,0, sizeof(float)*HISTN);
  memset(p->mean,0, sizeof(float)*MAXN*2);
  memset(p->var,0,  sizeof(float)*MAXN*2);
  dt_control_queue_redraw_widget(self->widget);
}

static void
acquire_button_pressed (GtkButton *button, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  // request color pick
  // needed to trigger expose events:
  self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
  dt_iop_colortransfer_params_t *p = (dt_iop_colortransfer_params_t *)self->params;
  p->flag = ACQUIRE;
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  dt_dev_add_history_item(darktable.develop, self, TRUE); // FIXME: Why do we need to add this to the history?
}

static void
apply_button_pressed (GtkButton *button, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colortransfer_params_t *p = (dt_iop_colortransfer_params_t *)self->params;
  dt_iop_colortransfer_gui_data_t *g = (dt_iop_colortransfer_gui_data_t *)self->gui_data;
  memcpy(p, &(g->flowback), self->params_size);
  p->flag = APPLY;
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static gboolean
expose (GtkWidget *widget, GdkEventExpose *event, dt_iop_module_t *self)
{
  // this is called whenever the pipeline finishes processing (i.e. after a color pick)
  if(darktable.gui->reset) return FALSE;
  dt_iop_colortransfer_params_t *p = (dt_iop_colortransfer_params_t *)self->params;
  if(p->flag == ACQUIRED)
  {
    // clear the color picking request if we got the cluster data
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    p->flag = NEUTRAL;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else if(p->flag == ACQUIRE2)
  {
    // color pick is still on, so the data has to be still in the pipe,
    // toggle a commit_params
    p->flag = ACQUIRE3;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  }
  return FALSE;
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colortransfer_params_t *p = (dt_iop_colortransfer_params_t *)p1;
  dt_iop_colortransfer_data_t *d = (dt_iop_colortransfer_data_t *)piece->data;
  if(p->flag == ACQUIRE3 && d->flag == ACQUIRED)
  {
    // if data is flagged ACQUIRED, actually copy data back from pipe!
    d->flag = NEUTRAL;
    p->flag = ACQUIRED; // let gui know the data is there.
    if(self->dev == darktable.develop && self->gui_data)
    {
      dt_iop_colortransfer_gui_data_t *g = (dt_iop_colortransfer_gui_data_t *)self->gui_data;
      memcpy (&g->flowback, d, self->params_size);
      g->flowback_set = 1;
      FILE *f = g_fopen("/tmp/dt_colortransfer_loaded", "wb");
      if(f)
      {
        if(fwrite(&g->flowback, self->params_size, 1, f) > 0) g->flowback.flag = APPLY;
        fclose(f);
      }
      dt_control_queue_redraw_widget(self->widget);
    }
  }
  else
  {
    // dt_iop_colortransfer_flag_t flag = d->flag;
    memcpy(d, p, self->params_size);
    // only allow apply and acquire commands from gui.
    // if(p->flag != APPLY && p->flag != ACQUIRE && p->flag != NEUTRAL) d->flag = flag;
    if(p->flag == ACQUIRE2) d->flag = ACQUIRE;
    if(p->flag == ACQUIRE3) d->flag = NEUTRAL;
    if(p->flag == ACQUIRED) d->flag = NEUTRAL;
    // if(p->flag == ACQUIRE) p->flag = ACQUIRE2;
  }
}
#endif

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_colortransfer_data_t));
  dt_iop_colortransfer_data_t *d = (dt_iop_colortransfer_data_t *)piece->data;
  d->flag = NEUTRAL;
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
#if 0
  dt_iop_colortransfer_params_t *p = (dt_iop_colortransfer_params_t *)self->params;
  dt_iop_colortransfer_gui_data_t *g = (dt_iop_colortransfer_gui_data_t *)self->gui_data;
  gtk_spin_button_set_value(g->spinbutton, p->n);
  //gtk_widget_set_size_request(GTK_WIDGET(g->area), 300, MIN(100, 300/p->n));
  // redraw color cluster preview
  dt_control_queue_redraw_widget(self->widget);
#endif
}

#if 0
static gboolean
cluster_preview_draw (GtkWidget *widget, cairo_t *crf, dt_iop_module_t *self)
{
  // dt_iop_colortransfer_params_t *p = (dt_iop_colortransfer_params_t *)self->params;
  dt_iop_colortransfer_gui_data_t *g = (dt_iop_colortransfer_gui_data_t *)self->gui_data;
  dt_iop_colortransfer_params_t *p = (dt_iop_colortransfer_params_t *)&g->flowback;
  if(!g->flowback_set) p = (dt_iop_colortransfer_params_t *)self->params;
  const int inset = 5;
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2*inset;
  height -= 2*inset;

  if(g->flowback_set) gtk_widget_set_sensitive(g->apply_button, TRUE);
#if 0
  if(g->flowback_set)
  {
    memcpy(self->params, &g->flowback, self->params_size);
    g->flowback_set = 0;
    p->flag = APPLY;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
#endif

  const float sep = 2.0;
  const float qwd = (width-(p->n-1)*sep)/(float)p->n;
  for(int cl=0; cl<p->n; cl++)
  {
    // draw cluster
    for(int j=-1; j<=1; j++) for(int i=-1; i<=1; i++)
      {
        // draw 9x9 grid showing mean and variance of this cluster.
        double rgb[3] = {0.5, 0.5, 0.5};
        cmsCIELab Lab;
        Lab.L = 5.0;//53.390011;
        Lab.a = (p->mean[cl][0] + i*p->var[cl][0]);// / Lab.L;
        Lab.b = (p->mean[cl][1] + j*p->var[cl][1]);// / Lab.L;
        Lab.L = 53.390011;
        cmsDoTransform(g->xform, &Lab, rgb, 1);
        cairo_set_source_rgb (cr, rgb[0], rgb[1], rgb[2]);
        cairo_rectangle(cr, qwd*(i+1)/3.0, height*(j+1)/3.0, qwd/3.0-.5, height/3.0-.5);
        cairo_fill(cr);
      }
    cairo_translate (cr, qwd + sep, 0);
  }

  cairo_destroy(cr);
  cairo_set_source_surface (crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}
#endif

void gui_init(struct dt_iop_module_t *self)
{
  IOP_GUI_ALLOC(colortransfer);

  self->widget = dt_ui_label_new(_("This module will be removed in the future\nand is only here so you can "
                                   "switch it off\nand move to the new color mapping module."));

#if 0
  dt_iop_colortransfer_gui_data_t *g = IOP_GUI_ALLOC(colortransfer);
  // dt_iop_colortransfer_params_t *p = (dt_iop_colortransfer_params_t *)self->params;

  g->flowback_set = 0;
  cmsHPROFILE hsRGB = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_IN)->profile;
  cmsHPROFILE hLab  = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;
  g->xform = cmsCreateTransform(hLab, TYPE_Lab_DBL, hsRGB, TYPE_RGB_DBL, INTENT_PERCEPTUAL, 0);

  self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  g_signal_connect (G_OBJECT(self->widget), "draw",
                    G_CALLBACK(draw), self);

  g->area = gtk_drawing_area_new();
  gtk_widget_set_size_request(GTK_WIDGET(g->area), 300, 100);
  gtk_box_pack_start(GTK_BOX(self->widget), g->area, TRUE, TRUE, 0);
  g_signal_connect (G_OBJECT (g->area), "draw", G_CALLBACK (cluster_preview_draw), self);

  GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 0);
  GtkWidget *button;
  g->spinbutton = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, MAXN, 1));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->spinbutton), _("Number of clusters to find in image"));
  gtk_box_pack_start(box, GTK_WIDGET(g->spinbutton), FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->spinbutton), "value-changed", G_CALLBACK(spinbutton_changed), (gpointer)self);

  button = gtk_button_new_with_label(_("Acquire"));
  g->acquire_button = button;
  gtk_widget_set_tooltip_text(button, _("Analyze this image"));
  gtk_box_pack_start(box, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(acquire_button_pressed), (gpointer)self);

  g->apply_button = gtk_button_new_with_label(_("Apply"));
  gtk_widget_set_tooltip_text(g->apply_button, _("Apply previously analyzed image look to this image"));
  gtk_box_pack_start(box, g->apply_button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->apply_button), "clicked", G_CALLBACK(apply_button_pressed), (gpointer)self);
  FILE *f = g_fopen("/tmp/dt_colortransfer_loaded", "rb");
  if(f)
  {
    if(fread(&g->flowback, self->params_size, 1, f) > 0) g->flowback_set = 1;
    fclose(f);
  }
  else gtk_widget_set_sensitive(GTK_WIDGET(g->apply_button), FALSE);
#endif
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
