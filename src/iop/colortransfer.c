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
 */

DT_MODULE(1)

typedef struct dt_iop_colortransfer_params_t
{
  // TODO: hist matching table?
  // TODO: n-means (max 5?) with mean/variance
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
{
  // TODO: same as params. (need duplicate because named preset contains params_t)
}
dt_iop_colortransfer_data_t;

const char *name()
{
  return _("color transfer");
}

#if 0 //histogram matching code
  int i,j,k,ch,value;
  const int g1 = 1, g2 = 3;
  // build separate histograms for each channel
  float hist[4][0x10000];
  bzero(hist, 4*0x10000*sizeof(float));
  // for(i=0;i<iwidth*iheight;i++) for(ch=0;ch<4;ch++)
  for(k=0;k<iheight;k++) for(i=0;i<iwidth;i++) // for(ch=0;ch<4;ch++)
  {
    // value = image[i][ch];
    // if(value) hist[ch][value]++;
    // if(fc(k,i)==1 || fc(k,i)==3) image[k*iwidth+i][ch] = 0;
    hist[fc(k,i)][image[k*iwidth+i][fc(k,i)]]++;
  }

  // combined histogram: this is the target distribution
  float histG12[0x10000];
  for(k=0;k<0x10000;k++) histG12[k] = hist[g1][k] + hist[g2][k];
  for(k=1;k<0x10000;k++) histG12[k] += histG12[k-1];
  const float histG12max = histG12[0xffff];

  for(i=0;i<1200;i++) printf("hist[%d] %f %f\n", i, hist[g1][i], hist[g2][i]);

  // accumulated start distribution of G1 G2
  for(k=1;k<0x10000;k++) hist[g1][k] += hist[g1][k-1];
  for(k=1;k<0x10000;k++) hist[g2][k] += hist[g2][k-1];
  const float histG1max = hist[g1][0xffff];
  const float histG2max = hist[g2][0xffff];

  printf("max: %f %f %f\n", histG1max, histG2max, histG12max);

  // normalize
  for(k=0;k<0x10000;k++) hist[g1][k] = hist[g1][k]*0xffff/histG1max;
  for(k=0;k<0x10000;k++) hist[g2][k] = hist[g2][k]*0xffff/histG2max;
  for(k=0;k<0x10000;k++) histG12 [k] = histG12 [k]*0xffff/histG12max;

  unsigned short int inv_histG12[0x10000];
  // invert normalised accumulated histG12
  int last = 0;
  for(i=0;i<0x10000;i++) for(k=last;k<0x10000;k++)
    if(histG12[k] >= i) { last = k; inv_histG12[i] = k; break; }

  for(i=0;i<1200;i++) printf("chist[%d] %f %f -- %d %d\n", i, hist[g1][i], hist[g2][i],
      inv_histG12[(int)(hist[g1][i])], inv_histG12[(int)(hist[g2][i])]);

  // now apply respective G and inv_G12
  for(k=0;k<iheight;k++) for(i=0;i<iwidth;i++) // for(ch=0;ch<4;ch++)
  {
    j = iwidth*k+i;
    // if(fc(k,i)==g1) printf("mapping %d -> %d\n", image[j][g1], inv_histG12[(int)(hist[g1][image[j][g1]])]);
    // if(fc(k,i)==g2) printf("mapping %d -> %d\n", image[j][g2], inv_histG12[(int)(hist[g2][image[j][g2]])]);
    if(fc(k,i)==g1) image[j][g1] = inv_histG12[(int)(hist[g1][image[j][g1]])];
    if(fc(k,i)==g2) image[j][g2] = inv_histG12[(int)(hist[g2][image[j][g2]])];
  }
#endif

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
  const int nit = 3; // number of iterations
  const int samples = roi->width*roi->height * 0.1; // samples: only a fraction of the buffer.
  // TODO: check if we need to go to Lab from L a/L B/L

  float mean[n][2], var[n][2];
  int cnt[n];

  // init n clusters for a, b channels at random
  for(int k=0;k<n;k++)
  {
    // TODO: range?
    mean_out[k][0] = dt_points_get();
    mean_out[k][1] = dt_points_get();
    var_out[k][0] = var_out[k][1] = 0.0f;
    mean[k][0] = mean[k][1] = val[k][0] = var[k][1] = 0.0f;
  }
  for(int it=0;it<nit;it++)
  {
    for(int k=0;k<n;k++) cnt[k] = 0;
    // randomly sample col positions inside roi
    for(int s=0;s<samples;s++)
    {
      const int j = dt_points_get()*roi->height, i = dt_points_get()*roi->width;
      // for each sample: determine cluster, update new mean, update var
      for(int k=0;k<n;k++)
      {
        // determine dist to mean_out
        const int c = get_cluster(col + 3*(roi->width*j + i), n, mean_out);
        cnt[c]++;
        // update mean, var
        var[c][0]  += col[3*(roi->width*j+i)+1]*col[3*(roi->width*j+i)+1];
        var[c][1]  += col[3*(roi->width*j+i)+2]*col[3*(roi->width*j+i)+2];
        mean[c][0] += col[3*(roi->width*j+i)+1];
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
      mean[k][0] = mean[k][1] = val[k][0] = var[k][1] = 0.0f;
    }
  }
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_colortransfer_data_t *data = (dt_iop_colortransfer_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;

  // TODO: acquire switch:
  // - capture button pressed: cluster preview buffer => gui style
  // - process: cluster only this buffer?? and reverse the effect using the style params
  //   => full buffer remains a coarse preview, export will work as expected.

  // L: match histogram

  // a, b: subtract mean, scale nvar/var, add nmean

}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colortransfer_params_t *p = (dt_iop_colortransfer_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[sharpen] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_colortransfer_data_t *d = (dt_iop_colortransfer_data_t *)piece->data;
  // TODO: copy the stuff
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_colortransfer_data_t));
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
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_colortransfer_gui_data_t *g = (dt_iop_colortransfer_gui_data_t *)self->gui_data;
  dt_iop_colortransfer_params_t *p = (dt_iop_colortransfer_params_t *)module->params;
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
  // TODO: sane default!
  dt_iop_colortransfer_params_t tmp = (dt_iop_colortransfer_params_t){};
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
  dt_iop_colortransfer_gui_data_t *g = (dt_iop_colortransfer_gui_data_t *)self->gui_data;
  dt_iop_colortransfer_params_t *p = (dt_iop_colortransfer_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  // TODO:
  // combo box with presets from db
  // capture button
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

