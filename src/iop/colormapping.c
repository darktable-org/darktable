/*
    This file is part of darktable,
    copyright (c) 2009--2013 johannes hanika.
    copyright (c) 2013 Ulrich Pegelow.

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
#include "common/bilateral.h"
#include "common/bilateralcl.h"
#include "common/colorspaces.h"
#include "common/opencl.h"
#include "common/points.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <gtk/gtk.h>
#include <inttypes.h>

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

DT_MODULE_INTROSPECTION(1, dt_iop_colormapping_params_t)

#define HISTN (1 << 11)
#define MAXN 5

typedef enum dt_iop_colormapping_flags_t
{
  NEUTRAL = 0,
  HAS_SOURCE = 1 << 0,
  HAS_TARGET = 1 << 1,
  ACQUIRE = 1 << 2,
  GET_SOURCE = 1 << 3,
  GET_TARGET = 1 << 4
} dt_iop_colormapping_flags_t;

typedef struct dt_iop_colormapping_flowback_t
{
  float hist[HISTN];
  // n-means (max 5?) with mean/variance
  float mean[MAXN][2];
  float var[MAXN][2];
  float weight[MAXN];
  // number of gaussians used.
  int n;
} dt_iop_colormapping_flowback_t;

typedef struct dt_iop_colormapping_params_t
{
  dt_iop_colormapping_flags_t flag;
  // number of gaussians used.
  int n;

  // relative importance of color dominance vs. color proximity
  float dominance;

  // level of histogram equalization
  float equalization;

  // hist matching table for source image
  float source_ihist[HISTN];
  // n-means (max 5) with mean/variance for source image
  float source_mean[MAXN][2];
  float source_var[MAXN][2];
  float source_weight[MAXN];

  // hist matching table for destination image
  int target_hist[HISTN];
  // n-means (max 5) with mean/variance for source image
  float target_mean[MAXN][2];
  float target_var[MAXN][2];
  float target_weight[MAXN];
} dt_iop_colormapping_params_t;

/** and pixelpipe data is just the same */
typedef struct dt_iop_colormapping_params_t dt_iop_colormapping_data_t;


typedef struct dt_iop_colormapping_gui_data_t
{
  int flag;
  float *buffer;
  int width;
  int height;
  int ch;
  int flowback_set;
  dt_iop_colormapping_flowback_t flowback;
  GtkWidget *acquire_source_button;
  GtkWidget *acquire_target_button;
  GtkWidget *source_area;
  GtkWidget *target_area;
  GtkWidget *clusters;
  GtkWidget *dominance;
  GtkWidget *equalization;
  cmsHTRANSFORM xform;
  dt_pthread_mutex_t lock;
} dt_iop_colormapping_gui_data_t;

typedef struct dt_iop_colormapping_global_data_t
{
  int kernel_histogram;
  int kernel_mapping;
} dt_iop_colormapping_global_data_t;


const char *name()
{
  return _("color mapping");
}

int groups()
{
  return IOP_GROUP_EFFECT;
}

int flags()
{
  return IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_SUPPORTS_BLENDING;
}


void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_iop(self, FALSE, NC_("accel", "acquire as source"), 0, 0);
  dt_accel_register_iop(self, FALSE, NC_("accel", "acquire as target"), 0, 0);
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_colormapping_gui_data_t *g = (dt_iop_colormapping_gui_data_t *)self->gui_data;

  dt_accel_connect_button_iop(self, "acquire as source", g->acquire_source_button);
  dt_accel_connect_button_iop(self, "acquire as target", g->acquire_target_button);
}


static void capture_histogram(const float *col, const int width, const int height, int *hist)
{
  // build separate histogram
  memset(hist, 0, HISTN * sizeof(int));
  for(int k = 0; k < height; k++)
    for(int i = 0; i < width; i++)
    {
      const int bin = CLAMP(HISTN * col[4 * (k * width + i) + 0] / 100.0, 0, HISTN - 1);
      hist[bin]++;
    }

  // accumulated start distribution of G1 G2
  for(int k = 1; k < HISTN; k++) hist[k] += hist[k - 1];
  for(int k = 0; k < HISTN; k++)
    hist[k] = (int)CLAMP(hist[k] * (HISTN / (float)hist[HISTN - 1]), 0, HISTN - 1);
  // for(int i=0;i<100;i++) printf("#[%d] %d \n", (int)CLAMP(HISTN*i/100.0, 0, HISTN-1),
  // hist[(int)CLAMP(HISTN*i/100.0, 0, HISTN-1)]);
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
  // for(int i=0;i<100;i++) printf("[%d] %f => %f\n", (int)CLAMP(HISTN*i/100.0, 0, HISTN-1),
  // hist[(int)CLAMP(HISTN*i/100.0, 0, HISTN-1)]/(float)HISTN, inv_hist[(int)CLAMP(HISTN*i/100.0, 0,
  // HISTN-1)]);
}

static void get_cluster_mapping(const int n, float mi[n][2], float wi[n], float mo[n][2], float wo[n],
                                const float dominance, int mapio[n])
{
  const float weightscale = 10000.0f;

  for(int ki = 0; ki < n; ki++)
  {
    // for each input cluster
    float mdist = FLT_MAX;
    for(int ko = 0; ko < n; ko++)
    {
      // find the best target cluster (the same could be used more than once)
      const float colordist = (mo[ko][0] - mi[ki][0]) * (mo[ko][0] - mi[ki][0])
                              + (mo[ko][1] - mi[ki][1]) * (mo[ko][1] - mi[ki][1]);
      const float weightdist = weightscale * (wo[ko] - wi[ki]) * (wo[ko] - wi[ki]);
      const float dist = colordist * (1.0f - dominance) + weightdist * dominance;
      if(dist < mdist)
      {
        // printf("[%d] => [%d] dominance: %f, colordist: %f, weightdist: %f, dist: %f\n", ki, ko, dominance,
        // colordist, weightdist, dist);
        mdist = dist;
        mapio[ki] = ko;
      }
    }
  }

  // printf("cluster mapping:\n");
  // for(int i=0;i<n;i++) printf("[%d] => [%d]\n", i, mapio[i]);
}


// inverse distant weighting according to D. Shepard's method; with power parameter 2.0
static void get_clusters(const float *col, const int n, float mean[n][2], float *weight)
{
  float mdist = FLT_MAX;
  for(int k = 0; k < n; k++)
  {
    const float dist2 = (col[1] - mean[k][0]) * (col[1] - mean[k][0])
                        + (col[2] - mean[k][1]) * (col[2] - mean[k][1]); // dist^2
    weight[k] = dist2 > 1.0e-6f ? 1.0f / dist2 : -1.0f;                  // direct hits marked as -1
    if(dist2 < mdist) mdist = dist2;
  }
  if(mdist < 1.0e-6f)
    for(int k = 0; k < n; k++)
      weight[k] = weight[k] < 0.0f ? 1.0f : 0.0f; // correction in case of direct hits
  float sum = 0.0f;
  for(int k = 0; k < n; k++) sum += weight[k];
  if(sum > 0.0f)
    for(int k = 0; k < n; k++) weight[k] /= sum;
}


static int get_cluster(const float *col, const int n, float mean[n][2])
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

static void kmeans(const float *col, const int width, const int height, const int n, float mean_out[n][2],
                   float var_out[n][2], float weight_out[n])
{
  const int nit = 40;                       // number of iterations
  const int samples = width * height * 0.2; // samples: only a fraction of the buffer.

  float mean[n][2], var[n][2];
  int cnt[n], count;

  float a_min = FLT_MAX, b_min = FLT_MAX, a_max = FLT_MIN, b_max = FLT_MIN;

  for(int s = 0; s < samples; s++)
  {
    const int j = CLAMP(dt_points_get() * height, 0, height - 1);
    const int i = CLAMP(dt_points_get() * width, 0, width - 1);

    const float a = col[4 * (width * j + i) + 1];
    const float b = col[4 * (width * j + i) + 2];

    a_min = fmin(a, a_min);
    a_max = fmax(a, a_max);
    b_min = fmin(b, b_min);
    b_max = fmax(b, b_max);
  }

  // init n clusters for a, b channels at random
  for(int k = 0; k < n; k++)
  {
    mean_out[k][0] = 0.9f * (a_min + (a_max - a_min) * dt_points_get());
    mean_out[k][1] = 0.9f * (b_min + (b_max - b_min) * dt_points_get());
    var_out[k][0] = var_out[k][1] = weight_out[k] = 0.0f;
    mean[k][0] = mean[k][1] = var[k][0] = var[k][1] = 0.0f;
  }
  for(int it = 0; it < nit; it++)
  {
    for(int k = 0; k < n; k++) cnt[k] = 0;
// randomly sample col positions inside roi
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(col, var, mean, mean_out, cnt)
#endif
    for(int s = 0; s < samples; s++)
    {
      const int j = CLAMP(dt_points_get() * height, 0, height - 1);
      const int i = CLAMP(dt_points_get() * width, 0, width - 1);
      // for each sample: determine cluster, update new mean, update var
      for(int k = 0; k < n; k++)
      {
        const float L = col[4 * (width * j + i)];
        const float Lab[3] = { L, col[4 * (width * j + i) + 1], col[4 * (width * j + i) + 2] };
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

    // determine weight of clusters
    count = 0;
    for(int k = 0; k < n; k++) count += cnt[k];
    for(int k = 0; k < n; k++) weight_out[k] = (count > 0) ? (float)cnt[k] / count : 0.0f;

    // printf("it %d  %d means:\n", it, n);
    // for(int k=0;k<n;k++) printf("mean %f %f -- var %f %f -- weight %f\n", mean_out[k][0], mean_out[k][1],
    // var_out[k][0], var_out[k][1], weight_out[k]);
  }

  for(int k = 0; k < n; k++)
  {
    // "eliminate" clusters with a variance of zero
    if(var_out[k][0] == 0.0f || var_out[k][1] == 0.0f)
      mean_out[k][0] = mean_out[k][1] = var_out[k][0] = var_out[k][1] = weight_out[k] = 0;

    // we actually want the std deviation.
    var_out[k][0] = sqrtf(var_out[k][0]);
    var_out[k][1] = sqrtf(var_out[k][1]);
  }

  // simple bubblesort of clusters in order of ascending weight: just a convenience for the user to keep
  // cluster display a bit more consistent in GUI
  for(int i = 0; i < n - 1; i++)
  {
    for(int j = 0; j < n - 1 - i; j++)
    {
      if(weight_out[j] > weight_out[j + 1])
      {
        float temp_mean[2] = { mean_out[j + 1][0], mean_out[j + 1][1] };
        float temp_var[2] = { var_out[j + 1][0], var_out[j + 1][1] };
        float temp_weight = weight_out[j + 1];

        mean_out[j + 1][0] = mean_out[j][0];
        mean_out[j + 1][1] = mean_out[j][1];
        var_out[j + 1][0] = var_out[j][0];
        var_out[j + 1][1] = var_out[j][1];
        weight_out[j + 1] = weight_out[j];

        mean_out[j][0] = temp_mean[0];
        mean_out[j][1] = temp_mean[1];
        var_out[j][0] = temp_var[0];
        var_out[j][1] = temp_var[1];
        weight_out[j] = temp_weight;
      }
    }
  }
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colormapping_data_t *data = (dt_iop_colormapping_data_t *)piece->data;
  dt_iop_colormapping_gui_data_t *g = (dt_iop_colormapping_gui_data_t *)self->gui_data;
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int ch = piece->colors;

  const float scale = piece->iscale / roi_in->scale;
  const float sigma_s = 50.0f / scale;
  const float sigma_r = 8.0f; // does not depend on scale

  // save a copy of preview input buffer so we can get histogram and color statistics out of it
  if(self->dev->gui_attached && g && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW && (data->flag & ACQUIRE))
  {
    dt_pthread_mutex_lock(&g->lock);
    if(g->buffer) free(g->buffer);

    g->buffer = malloc((size_t)width * height * ch * sizeof(float));
    g->width = width;
    g->height = height;
    g->ch = ch;

    if(g->buffer) memcpy(g->buffer, in, (size_t)width * height * ch * sizeof(float));

    dt_pthread_mutex_unlock(&g->lock);
  }

  // process image if all mapping information is present in the parameter set
  if(data->flag & HAS_TARGET && data->flag & HAS_SOURCE)
  {
    // for all pixels: find input cluster, transfer to mapped target cluster and apply histogram

    float dominance = data->dominance / 100.0f;
    float equalization = data->equalization / 100.0f;

    // get mapping from input clusters to target clusters
    int mapio[data->n];
    get_cluster_mapping(data->n, data->target_mean, data->target_weight, data->source_mean,
                        data->source_weight, dominance, mapio);

    float var_ratio[data->n][2];
    for(int i = 0; i < data->n; i++)
    {
      var_ratio[i][0]
          = (data->target_var[i][0] > 0.0f) ? data->source_var[mapio[i]][0] / data->target_var[i][0] : 0.0f;
      var_ratio[i][1]
          = (data->target_var[i][1] > 0.0f) ? data->source_var[mapio[i]][1] / data->target_var[i][1] : 0.0f;
    }

// first get delta L of equalized L minus original image L, scaled to fit into [0 .. 100]
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(data, in, out, equalization)
#endif
    for(int k = 0; k < height; k++)
    {
      size_t j = (size_t)ch * width * k;
      for(int i = 0; i < width; i++)
      {
        const float L = in[j];
        out[j] = 0.5f * ((L * (1.0f - equalization)
                          + data->source_ihist[data->target_hist[(int)CLAMP(
                                HISTN * L / 100.0f, 0.0f, (float)HISTN - 1.0f)]] * equalization) - L) + 50.0f;
        out[j] = CLAMP(out[j], 0.0f, 100.0f);
        j += ch;
      }
    }

    if(equalization > 0.001f)
    {
      // bilateral blur of delta L to avoid artifacts caused by limited histogram resolution
      dt_bilateral_t *b = dt_bilateral_init(width, height, sigma_s, sigma_r);
      if(!b) return;
      dt_bilateral_splat(b, out);
      dt_bilateral_blur(b);
      dt_bilateral_slice(b, out, out, -1.0f);
      dt_bilateral_free(b);
    }

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(data, in, out, var_ratio, mapio, equalization)
#endif
    for(int k = 0; k < height; k++)
    {
      float weight[data->n];
      size_t j = (size_t)ch * width * k;
      for(int i = 0; i < width; i++)
      {
        const float L = in[j];
        const float Lab[3] = { L, in[j + 1], in[j + 2] };

        // transfer back scaled and blurred delta L to output L
        out[j] = 2.0f * (out[j] - 50.0f) + L;
        out[j] = CLAMP(out[j], 0.0f, 100.0f);

        get_clusters(in + j, data->n, data->target_mean, weight);
        out[j + 1] = out[j + 2] = 0.0f;
        for(int c = 0; c < data->n; c++)
        {
          out[j + 1] += weight[c] * ((Lab[1] - data->target_mean[c][0]) * var_ratio[c][0]
                                     + data->source_mean[mapio[c]][0]);
          out[j + 2] += weight[c] * ((Lab[2] - data->target_mean[c][1]) * var_ratio[c][1]
                                     + data->source_mean[mapio[c]][1]);
        }
        out[j + 3] = in[j + 3];
        j += ch;
      }
    }
  }
  // incomplete parameter set -> do nothing
  else
  {
    memcpy(out, in, (size_t)sizeof(float) * ch * width * height);
  }
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colormapping_data_t *data = (dt_iop_colormapping_data_t *)piece->data;
  dt_iop_colormapping_global_data_t *gd = (dt_iop_colormapping_global_data_t *)self->data;
  dt_iop_colormapping_gui_data_t *g = (dt_iop_colormapping_gui_data_t *)self->gui_data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int ch = piece->colors;

  const float scale = piece->iscale / roi_in->scale;
  const float sigma_s = 50.0f / scale;
  const float sigma_r = 8.0f; // does not depend on scale

  float dominance = data->dominance / 100.0f;
  float equalization = data->equalization / 100.0f;

  dt_bilateral_cl_t *b = NULL;
  cl_mem dev_tmp = NULL;
  cl_mem dev_target_hist = NULL;
  cl_mem dev_source_ihist = NULL;
  cl_mem dev_target_mean = NULL;
  cl_mem dev_source_mean = NULL;
  cl_mem dev_var_ratio = NULL;
  cl_mem dev_mapio = NULL;


  // save a copy of preview input buffer so we can get histogram and color statistics out of it
  if(self->dev->gui_attached && g && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW && (data->flag & ACQUIRE))
  {
    dt_pthread_mutex_lock(&g->lock);
    free(g->buffer);

    g->buffer = malloc(width * height * ch * sizeof(float));
    g->width = width;
    g->height = height;
    g->ch = ch;

    if(g->buffer)
      err = dt_opencl_copy_device_to_host(devid, g->buffer, dev_in, width, height, ch * sizeof(float));

    dt_pthread_mutex_unlock(&g->lock);

    if(err != CL_SUCCESS) goto error;
  }


  // process image if all mapping information is present in the parameter set
  if(data->flag & HAS_TARGET && data->flag & HAS_SOURCE)
  {
    // get mapping from input clusters to target clusters
    int mapio[MAXN];
    get_cluster_mapping(data->n, data->target_mean, data->target_weight, data->source_mean,
                        data->source_weight, dominance, mapio);

    float var_ratio[MAXN][2];
    for(int i = 0; i < data->n; i++)
    {
      var_ratio[i][0]
          = (data->target_var[i][0] > 0.0f) ? data->source_var[mapio[i]][0] / data->target_var[i][0] : 0.0f;
      var_ratio[i][1]
          = (data->target_var[i][1] > 0.0f) ? data->source_var[mapio[i]][1] / data->target_var[i][1] : 0.0f;
    }

    dev_tmp = dt_opencl_alloc_device(devid, width, height, 4 * sizeof(float));
    if(dev_tmp == NULL) goto error;

    dev_target_hist = dt_opencl_copy_host_to_device_constant(devid, sizeof(int) * HISTN, data->target_hist);
    if(dev_target_hist == NULL) goto error;

    dev_source_ihist
        = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * HISTN, data->source_ihist);
    if(dev_source_ihist == NULL) goto error;

    dev_target_mean
        = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * MAXN * 2, data->target_mean);
    if(dev_target_mean == NULL) goto error;

    dev_source_mean
        = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * MAXN * 2, data->source_mean);
    if(dev_source_mean == NULL) goto error;

    dev_var_ratio = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * MAXN * 2, var_ratio);
    if(dev_var_ratio == NULL) goto error;

    dev_mapio = dt_opencl_copy_host_to_device_constant(devid, sizeof(int) * MAXN, mapio);
    if(dev_var_ratio == NULL) goto error;

    size_t sizes[3] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

    dt_opencl_set_kernel_arg(devid, gd->kernel_histogram, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_histogram, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_histogram, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_histogram, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_histogram, 4, sizeof(float), (void *)&equalization);
    dt_opencl_set_kernel_arg(devid, gd->kernel_histogram, 5, sizeof(cl_mem), (void *)&dev_target_hist);
    dt_opencl_set_kernel_arg(devid, gd->kernel_histogram, 6, sizeof(cl_mem), (void *)&dev_source_ihist);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_histogram, sizes);
    if(err != CL_SUCCESS) goto error;

    if(equalization > 0.001f)
    {
      b = dt_bilateral_init_cl(devid, width, height, sigma_s, sigma_r);
      if(!b) goto error;
      err = dt_bilateral_splat_cl(b, dev_out);
      if(err != CL_SUCCESS) goto error;
      err = dt_bilateral_blur_cl(b);
      if(err != CL_SUCCESS) goto error;
      err = dt_bilateral_slice_cl(b, dev_out, dev_tmp, -1.0f);
      if(err != CL_SUCCESS) goto error;
      dt_bilateral_free_cl(b);
      b = NULL; // make sure we don't clean it up twice
    }

    dt_opencl_set_kernel_arg(devid, gd->kernel_mapping, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_mapping, 1, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_mapping, 2, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_mapping, 3, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_mapping, 4, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_mapping, 5, sizeof(int), (void *)&data->n);
    dt_opencl_set_kernel_arg(devid, gd->kernel_mapping, 6, sizeof(cl_mem), (void *)&dev_target_mean);
    dt_opencl_set_kernel_arg(devid, gd->kernel_mapping, 7, sizeof(cl_mem), (void *)&dev_source_mean);
    dt_opencl_set_kernel_arg(devid, gd->kernel_mapping, 8, sizeof(cl_mem), (void *)&dev_var_ratio);
    dt_opencl_set_kernel_arg(devid, gd->kernel_mapping, 9, sizeof(cl_mem), (void *)&dev_mapio);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_mapping, sizes);
    if(err != CL_SUCCESS) goto error;

    dt_opencl_release_mem_object(dev_tmp);
    dt_opencl_release_mem_object(dev_target_hist);
    dt_opencl_release_mem_object(dev_source_ihist);
    dt_opencl_release_mem_object(dev_target_mean);
    dt_opencl_release_mem_object(dev_source_mean);
    dt_opencl_release_mem_object(dev_var_ratio);
    dt_opencl_release_mem_object(dev_mapio);
    return TRUE;
  }
  else
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }

error:
  if(b != NULL) dt_bilateral_free_cl(b);
  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  if(dev_target_hist != NULL) dt_opencl_release_mem_object(dev_target_hist);
  if(dev_source_ihist != NULL) dt_opencl_release_mem_object(dev_source_ihist);
  if(dev_target_mean != NULL) dt_opencl_release_mem_object(dev_target_mean);
  if(dev_source_mean != NULL) dt_opencl_release_mem_object(dev_source_mean);
  if(dev_var_ratio != NULL) dt_opencl_release_mem_object(dev_var_ratio);
  if(dev_mapio != NULL) dt_opencl_release_mem_object(dev_mapio);
  dt_print(DT_DEBUG_OPENCL, "[opencl_colormapping] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  const float scale = piece->iscale / roi_in->scale;
  const float sigma_s = 50.0f / scale;
  const float sigma_r = 8.0f; // does not depend on scale

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int channels = piece->colors;

  const size_t basebuffer = width * height * channels * sizeof(float);

  tiling->factor = 3.0f + (float)dt_bilateral_memory_use(width, height, sigma_s, sigma_r) / basebuffer;
  tiling->maxbuf
      = fmax(1.0f, (float)dt_bilateral_singlebuffer_size(width, height, sigma_s, sigma_r) / basebuffer);
  tiling->overhead = 0;
  tiling->overlap = ceilf(4 * sigma_s);
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colormapping_params_t *p = (dt_iop_colormapping_params_t *)p1;
  dt_iop_colormapping_data_t *d = (dt_iop_colormapping_data_t *)piece->data;

  memcpy(d, p, sizeof(dt_iop_colormapping_params_t));
#ifdef HAVE_OPENCL
  if(d->equalization > 0.1f)
    piece->process_cl_ready = (piece->process_cl_ready && !(darktable.opencl->avoid_atomics));
#endif
}


static void clusters_changed(GtkWidget *slider, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colormapping_params_t *p = (dt_iop_colormapping_params_t *)self->params;
  dt_iop_colormapping_gui_data_t *g = (dt_iop_colormapping_gui_data_t *)self->gui_data;

  int new = (int)dt_bauhaus_slider_get(slider);
  if(new != p->n)
  {
    p->n = new;
    memset(p->source_ihist, 0, sizeof(float) * HISTN);
    memset(p->source_mean, 0, sizeof(float) * MAXN * 2);
    memset(p->source_var, 0, sizeof(float) * MAXN * 2);
    memset(p->source_weight, 0, sizeof(float) * MAXN);
    memset(p->target_hist, 0, sizeof(int) * HISTN);
    memset(p->target_mean, 0, sizeof(float) * MAXN * 2);
    memset(p->target_var, 0, sizeof(float) * MAXN * 2);
    memset(p->target_weight, 0, sizeof(float) * MAXN);
    p->flag = NEUTRAL;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    dt_control_queue_redraw_widget(g->source_area);
    dt_control_queue_redraw_widget(g->target_area);
  }
}

static void dominance_changed(GtkWidget *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_colormapping_params_t *p = (dt_iop_colormapping_params_t *)self->params;
  p->dominance = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void equalization_changed(GtkWidget *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_colormapping_params_t *p = (dt_iop_colormapping_params_t *)self->params;
  p->equalization = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void acquire_source_button_pressed(GtkButton *button, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colormapping_params_t *p = (dt_iop_colormapping_params_t *)self->params;
  p->flag |= ACQUIRE;
  p->flag |= GET_SOURCE;
  p->flag &= ~HAS_SOURCE;
  dt_iop_request_focus(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void acquire_target_button_pressed(GtkButton *button, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colormapping_params_t *p = (dt_iop_colormapping_params_t *)self->params;
  p->flag |= ACQUIRE;
  p->flag |= GET_TARGET;
  p->flag &= ~HAS_TARGET;
  dt_iop_request_focus(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_colormapping_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_colormapping_params_t *p = (dt_iop_colormapping_params_t *)self->params;
  dt_iop_colormapping_gui_data_t *g = (dt_iop_colormapping_gui_data_t *)self->gui_data;
  dt_bauhaus_slider_set(g->clusters, p->n);
  dt_bauhaus_slider_set(g->dominance, p->dominance);
  dt_bauhaus_slider_set(g->equalization, p->equalization);
  dt_control_queue_redraw_widget(self->widget);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_colormapping_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_colormapping_params_t));
  module->default_enabled = 0;
  module->priority = 492; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_colormapping_params_t);
  module->gui_data = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl, from programs.conf
  dt_iop_colormapping_global_data_t *gd
      = (dt_iop_colormapping_global_data_t *)malloc(sizeof(dt_iop_colormapping_global_data_t));
  module->data = gd;
  gd->kernel_histogram = dt_opencl_create_kernel(program, "colormapping_histogram");
  gd->kernel_mapping = dt_opencl_create_kernel(program, "colormapping_mapping");
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colormapping_global_data_t *gd = (dt_iop_colormapping_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_histogram);
  dt_opencl_free_kernel(gd->kernel_mapping);
  free(module->data);
  module->data = NULL;
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_colormapping_params_t tmp
      = (dt_iop_colormapping_params_t){ .flag = NEUTRAL, .n = 3, .dominance = 100.0f, .equalization = 50.0f };

  // we might be called from presets update infrastructure => there is no image
  if(!module->dev) goto end;

  dt_iop_colormapping_gui_data_t *g = (dt_iop_colormapping_gui_data_t *)module->gui_data;
  if(module->dev->gui_attached && g && g->flowback_set)
  {
    memcpy(tmp.source_ihist, g->flowback.hist, sizeof(float) * HISTN);
    memcpy(tmp.source_mean, g->flowback.mean, sizeof(float) * MAXN * 2);
    memcpy(tmp.source_var, g->flowback.var, sizeof(float) * MAXN * 2);
    memcpy(tmp.source_weight, g->flowback.weight, sizeof(float) * MAXN);
    tmp.n = g->flowback.n;
    tmp.flag = HAS_SOURCE;
  }
  module->default_enabled = 0;

end:
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colormapping_params_t));
  memcpy(module->params, &tmp, sizeof(dt_iop_colormapping_params_t));
}


static gboolean cluster_preview_draw(GtkWidget *widget, cairo_t *crf, dt_iop_module_t *self)
{
  dt_iop_colormapping_params_t *p = (dt_iop_colormapping_params_t *)self->params;
  dt_iop_colormapping_gui_data_t *g = (dt_iop_colormapping_gui_data_t *)self->gui_data;

  float(*mean)[2];
  float(*var)[2];

  if(widget == g->source_area)
  {
    mean = p->source_mean;
    var = p->source_var;
  }
  else
  {
    mean = p->target_mean;
    var = p->target_var;
  }


  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int inset = 5;
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  cairo_set_source_rgb(cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2 * inset;
  height -= 2 * inset;


  const float sep = DT_PIXEL_APPLY_DPI(2.0);
  const float qwd = (width - (p->n - 1) * sep) / (float)p->n;
  for(int cl = 0; cl < p->n; cl++)
  {
    // draw cluster
    for(int j = -1; j <= 1; j++)
      for(int i = -1; i <= 1; i++)
      {
        // draw 9x9 grid showing mean and variance of this cluster.
        double rgb[3] = { 0.5, 0.5, 0.5 };
        cmsCIELab Lab;
        Lab.L = 5.0; // 53.390011;
        Lab.a = (mean[cl][0] + i * var[cl][0]); // / Lab.L;
        Lab.b = (mean[cl][1] + j * var[cl][1]); // / Lab.L;
        Lab.L = 53.390011;
        cmsDoTransform(g->xform, &Lab, rgb, 1);
        cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
        cairo_rectangle(cr, qwd * (i + 1) / 3.0, height * (j + 1) / 3.0, qwd / 3.0 - DT_PIXEL_APPLY_DPI(.5),
                        height / 3.0 - DT_PIXEL_APPLY_DPI(.5));
        cairo_fill(cr);
      }
    cairo_translate(cr, qwd + sep, 0);
  }

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}


static void process_clusters(gpointer instance, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colormapping_params_t *p = (dt_iop_colormapping_params_t *)self->params;
  dt_iop_colormapping_gui_data_t *g = (dt_iop_colormapping_gui_data_t *)self->gui_data;
  int new_source_clusters = 0;

  if(!g || !g->buffer) return;
  if(!(p->flag & ACQUIRE)) return;

  darktable.gui->reset = 1;

  dt_pthread_mutex_lock(&g->lock);
  const int width = g->width;
  const int height = g->height;
  const int ch = g->ch;
  float *buffer = malloc(width * height * ch * sizeof(float));
  if(!buffer)
  {
    dt_pthread_mutex_unlock(&g->lock);
    return;
  }
  memcpy(buffer, g->buffer, width * height * ch * sizeof(float));
  dt_pthread_mutex_unlock(&g->lock);

  if(p->flag & GET_SOURCE)
  {
    int hist[HISTN];

    // get histogram of L
    capture_histogram(buffer, width, height, hist);

    // invert histogram
    invert_histogram(hist, p->source_ihist);

    // get n color clusters
    kmeans(buffer, width, height, p->n, p->source_mean, p->source_var, p->source_weight);

    p->flag |= HAS_SOURCE;
    new_source_clusters = 1;

    dt_control_queue_redraw_widget(g->source_area);
  }
  else if(p->flag & GET_TARGET)
  {
    // get histogram of L
    capture_histogram(buffer, width, height, p->target_hist);

    // get n color clusters
    kmeans(buffer, width, height, p->n, p->target_mean, p->target_var, p->target_weight);

    p->flag |= HAS_TARGET;

    dt_control_queue_redraw_widget(g->target_area);
  }

  free(buffer);

  if(new_source_clusters)
  {
    memcpy(g->flowback.hist, p->source_ihist, sizeof(float) * HISTN);
    memcpy(g->flowback.mean, p->source_mean, sizeof(float) * MAXN * 2);
    memcpy(g->flowback.var, p->source_var, sizeof(float) * MAXN * 2);
    memcpy(g->flowback.weight, p->source_weight, sizeof(float) * MAXN);
    g->flowback.n = p->n;
    g->flowback_set = 1;
    FILE *f = fopen("/tmp/dt_colormapping_loaded", "wb");
    if(f)
    {
      if(fwrite(&g->flowback, sizeof(g->flowback), 1, f) < 1)
        fprintf(stderr, "[colormapping] could not write flowback file /tmp/dt_colormapping_loaded\n");
      fclose(f);
    }
  }

  p->flag &= ~(GET_TARGET | GET_SOURCE | ACQUIRE);
  darktable.gui->reset = 0;

  if(p->flag & HAS_SOURCE) dt_dev_add_history_item(darktable.develop, self, TRUE);

  dt_control_queue_redraw();
}


void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_colormapping_gui_data_t));
  dt_iop_colormapping_gui_data_t *g = (dt_iop_colormapping_gui_data_t *)self->gui_data;
  dt_iop_colormapping_params_t *p = (dt_iop_colormapping_params_t *)self->params;

  g->flag = NEUTRAL;
  g->flowback_set = 0;
  cmsHPROFILE hsRGB = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_DIRECTION_IN)->profile;
  cmsHPROFILE hLab = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;
  g->xform = cmsCreateTransform(hLab, TYPE_Lab_DBL, hsRGB, TYPE_RGB_DBL, INTENT_PERCEPTUAL, 0);
  g->buffer = NULL;

  dt_pthread_mutex_init(&g->lock, NULL);

  self->widget = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));

  GtkBox *hbox1 = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  GtkWidget *source = gtk_label_new(_("source clusters:"));
  gtk_box_pack_start(GTK_BOX(hbox1), GTK_WIDGET(source), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox1), TRUE, TRUE, 0);

  g->source_area = dtgtk_drawing_area_new_with_aspect_ratio(1.0 / 3.0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->source_area, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->source_area), "draw", G_CALLBACK(cluster_preview_draw), self);

  GtkBox *hbox2 = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  GtkWidget *target = gtk_label_new(_("target clusters:"));
  gtk_box_pack_start(GTK_BOX(hbox2), GTK_WIDGET(target), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox2), TRUE, TRUE, 0);

  g->target_area = dtgtk_drawing_area_new_with_aspect_ratio(1.0 / 3.0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->target_area, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->target_area), "draw", G_CALLBACK(cluster_preview_draw), self);


  GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 0);
  GtkWidget *button;

  button = gtk_button_new_with_label(_("acquire as source"));
  g->acquire_source_button = button;
  gtk_widget_set_tooltip_text(button, _("analyze this image as a source image"));
  gtk_box_pack_start(box, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(acquire_source_button_pressed), (gpointer)self);

  button = gtk_button_new_with_label(_("acquire as target"));
  g->acquire_target_button = button;
  gtk_widget_set_tooltip_text(button, _("analyze this image as a target image"));
  gtk_box_pack_start(box, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(acquire_target_button_pressed), (gpointer)self);

  g->clusters = dt_bauhaus_slider_new_with_range(self, 1.0f, 5.0f, 1., p->n, 0);
  dt_bauhaus_widget_set_label(g->clusters, NULL, _("number of clusters"));
  dt_bauhaus_slider_set_format(g->clusters, "%.0f");
  gtk_widget_set_tooltip_text(g->clusters, _("number of clusters to find in image. value change resets all clusters"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->clusters), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->clusters), "value-changed", G_CALLBACK(clusters_changed), (gpointer)self);

  g->dominance = dt_bauhaus_slider_new_with_range(self, 0, 100.0, 2., p->dominance, 2);
  dt_bauhaus_widget_set_label(g->dominance, NULL, _("color dominance"));
  gtk_widget_set_tooltip_text(g->dominance, _("how clusters are mapped. low values: based on color "
                                              "proximity, high values: based on color dominance"));
  dt_bauhaus_slider_set_format(g->dominance, "%.02f%%");
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->dominance), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->dominance), "value-changed", G_CALLBACK(dominance_changed), self);

  g->equalization = dt_bauhaus_slider_new_with_range(self, 0, 100.0, 2., p->equalization, 2);
  dt_bauhaus_widget_set_label(g->equalization, NULL, _("histogram equalization"));
  gtk_widget_set_tooltip_text(g->equalization, _("level of histogram equalization"));
  dt_bauhaus_slider_set_format(g->equalization, "%.02f%%");
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->equalization), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->equalization), "value-changed", G_CALLBACK(equalization_changed), self);

  /* add signal handler for preview pipe finished: process clusters if requested */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                            G_CALLBACK(process_clusters), self);


  FILE *f = fopen("/tmp/dt_colormapping_loaded", "rb");
  if(f)
  {
    if(fread(&g->flowback, sizeof(g->flowback), 1, f) > 0) g->flowback_set = 1;
    fclose(f);
  }
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colormapping_gui_data_t *g = (dt_iop_colormapping_gui_data_t *)self->gui_data;

  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(process_clusters), self);

  cmsDeleteTransform(g->xform);
  dt_pthread_mutex_destroy(&g->lock);
  free(g->buffer);
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
