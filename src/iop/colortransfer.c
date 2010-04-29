/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <strings.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#ifdef HAVE_GEGL
  #include <gegl.h>
#endif
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "common/points.h"
#include "gui/gtk.h"


/**
 * color transfer somewhat based on the glorious paper `color transfer between images'
 * by erik reinhard, michael ashikhmin, bruce gooch, and peter shirley, 2001.
 * chosen because it officially cites the playboy.
 *
 * data flow:
 * 1) button is pressed: request color pick and set to acquire mode, add history item
 * 2) preview is processed, acquires data and sets data to acquired
 * 3) expose gets the post-process callback, launches another commit_params via add history item
 * 4) commit_params copies data back to gui params and sets params flag to acquired.
 */

DT_MODULE(1)

#define HISTN (1<<11)
#define MAXN 5

typedef enum dt_iop_colortransfer_flag_t
{
  ACQUIRE = 0,
  ACQUIRED = 1,
  APPLY = 2,
  NEUTRAL = 3
}
dt_iop_colortransfer_flag_t;

typedef struct dt_iop_colortransfer_params_t
{
  dt_iop_colortransfer_flag_t flag;
  // hist matching table
  float hist[HISTN];
  // n-means (max 5?) with mean/variance
  float mean[MAXN][2];
  float var [MAXN][2];
  // number of gaussians used.
  int n;
}
dt_iop_colortransfer_params_t;

typedef struct dt_iop_colortransfer_gui_data_t
{
  // TODO: button to acquire new style data (as exposure/spot wb)
  // TODO: list of currently applied is maintained as preset (right click)
  // TODO: maybe provide an additional combo box with the same presets?
}
dt_iop_colortransfer_gui_data_t;

typedef struct dt_iop_colortransfer_data_t
{ // same as params. (need duplicate because database table preset contains params_t)
  dt_iop_colortransfer_flag_t flag;
  float hist[HISTN];
  float mean[MAXN][2];
  float var [MAXN][2];
  int n;
}
dt_iop_colortransfer_data_t;

const char *name()
{
  return _("color transfer");
}

static void
capture_histogram(const float *col, const dt_iop_roi_t *roi, int *hist)
{
  // build separate histogram
  bzero(hist, HISTN*sizeof(float));
  for(int k=0;k<roi->height;k++) for(int i=0;i<roi->width;i++)
  {
    const int bin = CLAMP(HISTN*col[3*(k*roi->width+i)+0]/100.0, 0, HISTN-1);
    hist[bin]++;
  }

  // accumulated start distribution of G1 G2
  for(int k=1;k<HISTN;k++) hist[k] += hist[k-1];

  // TODO: remove!
  // const float hist_max = hist[HISTN-1];
  // normalise
  // for(int k=0;k<HISTN;k++) hist[k] = hist[k]*(HISTN-1)/hist_max;
}

static void
invert_histogram(const int *hist, float *inv_hist)
{
  // invert non-normalised accumulated hist
  int last = 0;
  for(int i=0;i<HISTN;i++) for(int k=last;k<HISTN;k++)
    if(hist[k]*HISTN >= i*hist[HISTN-1]) { last = k; inv_hist[i] = k/(float)HISTN; break; }
}

static void
get_cluster_mapping(const int n, float mi[n][2], float mo[n][2], int mapio[n])
{
  for(int ki=0;ki<n;ki++)
  { // for each input cluster
    float mdist = FLT_MAX;
    for(int ko=0;ko<n;ko++)
    { // find the best target cluster (the same could be used more than once)
      const float dist = (mo[ko][0]-mi[ki][0])*(mo[ko][0]-mi[ki][0])+(mo[ko][1]-mi[ki][1])*(mo[ko][1]-mi[ki][1]);
      if(dist < mdist)
      {
        mdist = dist;
        mapio[ki] = ko;
      }
    }
  }
}

static int
get_cluster(const float *col, const int n, float mean[n][2])
{
  float mdist = FLT_MAX;
  int cluster = 0;
  for(int k=0;k<n;k++)
  {
    const float dist = (col[1]-mean[k][0])*(col[1]-mean[k][0]) + (col[2]-mean[k][1])*(col[2]-mean[k][1]);
    if(dist < mdist)
    {
      mdist = dist;
      cluster = k;
    }
  }
  return cluster;
}

static void
kmeans(const float *col, const dt_iop_roi_t *roi, const int n, float mean_out[n][2], float var_out[n][2])
{
  // TODO: check params here:
  const int nit = 10; // number of iterations
  const int samples = roi->width*roi->height * 0.1; // samples: only a fraction of the buffer.
  // TODO: check if we need to go to Lab from L a/L b/L

  float mean[n][2], var[n][2];
  int cnt[n];

  // init n clusters for a, b channels at random
  for(int k=0;k<n;k++)
  {
    // TODO: range?
    mean_out[k][0] = 20.0f-40.0f*dt_points_get();
    mean_out[k][1] = 20.0f-40.0f*dt_points_get();
    var_out[k][0] = var_out[k][1] = 0.0f;
    mean[k][0] = mean[k][1] = var[k][0] = var[k][1] = 0.0f;
  }
  for(int it=0;it<nit;it++)
  {
    for(int k=0;k<n;k++) cnt[k] = 0;
    // randomly sample col positions inside roi
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static) shared(roi,col,var,mean,mean_out,cnt)
#endif
    for(int s=0;s<samples;s++)
    {
      const int j = dt_points_get()*roi->height, i = dt_points_get()*roi->width;
      // for each sample: determine cluster, update new mean, update var
      for(int k=0;k<n;k++)
      {
        // determine dist to mean_out
        const int c = get_cluster(col + 3*(roi->width*j + i), n, mean_out);
#ifdef _OPENMP
  #pragma omp atomic
#endif
        cnt[c]++;
        // update mean, var
#ifdef _OPENMP
  #pragma omp atomic
#endif
        var[c][0]  += col[3*(roi->width*j+i)+1]*col[3*(roi->width*j+i)+1];
#ifdef _OPENMP
  #pragma omp atomic
#endif
        var[c][1]  += col[3*(roi->width*j+i)+2]*col[3*(roi->width*j+i)+2];
#ifdef _OPENMP
  #pragma omp atomic
#endif
        mean[c][0] += col[3*(roi->width*j+i)+1];
#ifdef _OPENMP
  #pragma omp atomic
#endif
        mean[c][1] += col[3*(roi->width*j+i)+2];
      }
    }
    // swap old/new means
    for(int k=0;k<n;k++)
    {
      mean_out[k][0] = mean[k][0]/cnt[k];
      mean_out[k][1] = mean[k][1]/cnt[k];
      var_out[k][0] = var[k][0]/cnt[k] - mean_out[k][0]*mean_out[k][0];
      var_out[k][1] = var[k][1]/cnt[k] - mean_out[k][1]*mean_out[k][1];
      mean[k][0] = mean[k][1] = var[k][0] = var[k][1] = 0.0f;
    }
    printf("it %d  %d means:\n", it, n);
    for(int k=0;k<n;k++) printf("%f %f -- var %f %f\n", mean_out[k][0], mean_out[k][1], var_out[k][0], var_out[k][1]);
  }
  for(int k=0;k<n;k++)
  { // we actually want the std deviation.
    var_out[k][0] = sqrtf(var_out[k][0]);
    var_out[k][1] = sqrtf(var_out[k][1]);
  }
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_colortransfer_data_t *data = (dt_iop_colortransfer_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;

  printf("process: flag %d\n", data->flag);
  // TODO: acquire switch:
  // - capture button pressed: cluster preview buffer => gui style
  // - process: cluster only this buffer?? and reverse the effect using the style params
  //   => full buffer remains a coarse preview, export will work as expected.
  if(self->request_color_pick)
  // if(data->flag == ACQUIRE)
  {
    printf("acuiring..\n");
    if(piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
    { // only get stuff from the preview pipe, rest stays untouched.
      int hist[HISTN];
      // get histogram of L
      capture_histogram(in, roi_in, hist);
      // invert histogram of L
      invert_histogram(hist, data->hist);

      // get n clusters
      kmeans(in, roi_in, data->n, data->mean, data->var);

      printf("acquired %d means:\n", data->n);
      for(int k=0;k<data->n;k++) printf("%f %f -- var %f %f\n", data->mean[k][0], data->mean[k][1], data->var[k][0], data->var[k][1]);

      // notify gui that commit_params should let stuff flow back!
      data->flag = ACQUIRED;
    }
    memcpy(out, in, sizeof(float)*3*roi_out->width*roi_out->height);
  }
  else if(data->flag == APPLY)
  { // apply histogram of L and clustering of (a,b)
    printf("applying stuff because flag=%d\n", data->flag);
    int hist[HISTN];
    capture_histogram(in, roi_in, hist);
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static) shared(roi_out,data,in,out,hist)
#endif
    for(int k=0;k<roi_out->height;k++)
    {
      int j = 3*roi_out->width*k;
      for(int i=0;i<roi_out->width;i++)
      { // L: match histogram
        out[j] = data->hist[hist[(int)CLAMP(HISTN*in[j], 0, HISTN-1)]];
        j+=3;
      }
    }

    // cluster input buffer
    float mean[data->n][2], var[data->n][2];
    kmeans(in, roi_in, data->n, mean, var);
    
    // get mapping from input clusters to target clusters
    int mapio[data->n];
    get_cluster_mapping(data->n, mean, data->mean, mapio);

    // for all pixels: find input cluster, transfer to mapped target cluster
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static) shared(roi_out,data,mean,var,mapio,in,out)
#endif
    for(int k=0;k<roi_out->height;k++)
    {
      int j = 3*roi_out->width*k;
      for(int i=0;i<roi_out->width;i++)
      {
        const int ki = get_cluster(in + j, data->n, mean);
        // a, b: subtract mean, scale nvar/var, add nmean
        out[j+1] = (in[j+1] - mean[ki][0])*data->var[mapio[ki]][0]/var[ki][0] + data->mean[mapio[ki]][0];
        out[j+2] = (in[j+2] - mean[ki][1])*data->var[mapio[ki]][1]/var[ki][1] + data->mean[mapio[ki]][1];
        j+=3;
      }
    }
  }
  else
  {
    printf("doing nothing because flag=%d\n", data->flag);
    memcpy(out, in, sizeof(float)*3*roi_out->width*roi_out->height);
  }
}

static void
button_pressed (GtkButton *button, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  // request color pick
  self->request_color_pick = 1;
  dt_iop_colortransfer_params_t *p = (dt_iop_colortransfer_params_t *)self->params;
  printf("setting flag to acquire\n");
  p->flag = ACQUIRE;
  // dt_dev_add_history_item(darktable.develop, self);
}

static gboolean
expose (GtkWidget *widget, GdkEventExpose *event, dt_iop_module_t *self)
{ // this is called whenever the pipeline finishes processing (i.e. after a color pick)
  if(darktable.gui->reset) return FALSE;
  if(!self->request_color_pick) return FALSE;
  dt_iop_colortransfer_params_t *p = (dt_iop_colortransfer_params_t *)self->params;
  if(p->flag == ACQUIRED)
  { // clear the color picking request if we got the cluster data
    printf("expose: all done!\n");
    self->request_color_pick = 0;
  }
  else
  { // color pick is still on, so the data has to be still in the pipe,
    // toggle a commit_params
    printf("expose: triggering another pass\n");
    p->flag = NEUTRAL;
    dt_dev_add_history_item(darktable.develop, self);
  }
  return FALSE;
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colortransfer_params_t *p = (dt_iop_colortransfer_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[colortransfer] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_colortransfer_data_t *d = (dt_iop_colortransfer_data_t *)piece->data;
  printf("commit params: p %d  d %d\n", p->flag, d->flag);
  if(p->flag == ACQUIRE && d->flag == ACQUIRED)
  { // if data is flagged ACQUIRED, actually copy data back from pipe!
    printf("commit params: copying back data\n");
    memcpy (p, d, self->params_size);
    d->flag = NEUTRAL;
    p->flag = ACQUIRED; // let gui know the data is there.
  }
  else
  {
    printf("commit params: copying data from params.\n");
    dt_iop_colortransfer_flag_t flag = d->flag;
    memcpy(d, p, self->params_size);
    // only allow apply and acquire commands from gui.
    if(p->flag != APPLY && p->flag != ACQUIRE) d->flag = flag;
  }
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_colortransfer_data_t));
  dt_iop_colortransfer_data_t *d = (dt_iop_colortransfer_data_t *)piece->data;
  d->flag = NEUTRAL;
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // clean up everything again.
  (void)gegl_node_remove_child(pipe->gegl, piece->input);
  // no free necessary, no data is alloc'ed
#else
  free(piece->data);
#endif
}

void gui_update(struct dt_iop_module_t *self)
{
  // dt_iop_module_t *module = (dt_iop_module_t *)self;
  // dt_iop_colortransfer_gui_data_t *g = (dt_iop_colortransfer_gui_data_t *)self->gui_data;
  // dt_iop_colortransfer_params_t *p = (dt_iop_colortransfer_params_t *)module->params;
  // TODO: find active preset?
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_colortransfer_data_t));
  module->params = malloc(sizeof(dt_iop_colortransfer_params_t));
  module->default_params = malloc(sizeof(dt_iop_colortransfer_params_t));
  module->default_enabled = 0;
  module->priority = 850;
  module->params_size = sizeof(dt_iop_colortransfer_params_t);
  module->gui_data = NULL;
  dt_iop_colortransfer_params_t tmp;
  tmp.flag = NEUTRAL;
  bzero(tmp.hist, sizeof(float)*HISTN);
  bzero(tmp.mean, sizeof(float)*MAXN*2);
  bzero(tmp.var,  sizeof(float)*MAXN*2);
  tmp.n = 3;
  memcpy(module->params, &tmp, sizeof(dt_iop_colortransfer_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colortransfer_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_colortransfer_gui_data_t));
  // dt_iop_colortransfer_gui_data_t *g = (dt_iop_colortransfer_gui_data_t *)self->gui_data;
  // dt_iop_colortransfer_params_t *p = (dt_iop_colortransfer_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(TRUE, 5));
  g_signal_connect (G_OBJECT(self->widget), "expose-event",
                    G_CALLBACK(expose), self);

  GtkWidget *button;
  button = gtk_button_new_with_label(_("capture color statistics"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("analyze this image and store parameters"), NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_pressed), (gpointer)self);

  button = gtk_button_new_with_label(_("apply"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("apply color statistics to this image"), NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), button, TRUE, TRUE, 0);
  // TODO:
  // combo box with presets from db
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

#undef HISTN
#undef MAXN

