/*
    This file is part of darktable,
    Copyright (C) 2013-2023 darktable developers.

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
#include "common/imagebuf.h"
#include "common/math.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "gui/gtk.h"
#include "gui/accelerators.h"
#include "iop/iop_api.h"
#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(1, dt_iop_defringe_params_t)

typedef enum dt_iop_defringe_mode_t
{
  MODE_GLOBAL_AVERAGE = 0, // $DESCRIPTION: "global average (fast)"
  MODE_LOCAL_AVERAGE = 1,  // $DESCRIPTION: "local average (slow)"
  MODE_STATIC = 2          // $DESCRIPTION: "static threshold (fast)"
} dt_iop_defringe_mode_t;

typedef struct dt_iop_defringe_params_t
{
  float radius; // $MIN: 0.5 $MAX: 20.0 $DEFAULT: 4.0 $DESCRIPTION: "edge detection radius"
  float thresh; // $MIN: 0.5 $MAX: 128.0 $DEFAULT: 20.0 $DESCRIPTION: "threshold"
  dt_iop_defringe_mode_t op_mode; // $DEFAULT: MODE_GLOBAL_AVERAGE $DESCRIPTION: "operation mode"
} dt_iop_defringe_params_t;

typedef dt_iop_defringe_params_t dt_iop_defringe_data_t;

typedef struct dt_iop_defringe_gui_data_t
{
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

const char *aliases()
{
  return _("chromatic aberrations");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("attenuate chromatic aberration by desaturating edges"),
                                      _("corrective"),
                                      _("linear or non-linear, Lab, display-referred"),
                                      _("non-linear, Lab"),
                                      _("non-linear, Lab, display-referred"));
}

int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_TECHNICAL | IOP_FLAGS_DEPRECATED;
}

int flags()
{
  // a second instance might help to reduce artifacts when thick fringe needs to be removed
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

const char *deprecated_msg()
{
  return _("this module is deprecated. please use the chromatic aberration module instead.");
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

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
#ifdef HAVE_OPENCL
  tiling->factor_cl = 2.0f + (float)dt_gaussian_memory_use_cl(width, height, channels)/basebuffer;
#endif
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
  dt_iop_defringe_data_t *const d = (dt_iop_defringe_data_t *)piece->data;
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, module, piece->colors,
                                         i, o, roi_in, roi_out))
    return; // image has been copied through to output and module's trouble flag has been updated

  const int order = 1; // 0,1,2
  const float sigma = fmax(0.1f, fabs(d->radius)) * roi_in->scale / piece->iscale;
  const float Labmax[] = { 100.0f, 128.0f, 128.0f, 1.0f };
  const float Labmin[] = { 0.0f, -128.0f, -128.0f, 0.0f };
  const int ch = 4;
  const int radius = ceil(2.0 * ceilf(sigma));

  // save the fibonacci lattices in them later
  int *xy_avg = NULL;
  int *xy_small = NULL;

  if(roi_out->width < 2 * radius + 1 || roi_out->height < 2 * radius + 1) goto ERROR_EXIT;

  float avg_edge_chroma = 0.0;

  const float *const restrict in = (float *const)i;
  float *const restrict out = (float *const)o;
  const int width = roi_in->width;
  const int height = roi_in->height;

  dt_gaussian_t *gauss = NULL;
  gauss = dt_gaussian_init(width, height, 4, Labmax, Labmin, sigma, order);
  if(!gauss)
  {
    fprintf(stderr, "Error allocating memory for gaussian blur in: defringe module\n");
    goto ERROR_EXIT;
  }
  dt_gaussian_blur_4c(gauss, in, out);
  dt_gaussian_free(gauss);

  const int samples_wish = radius * radius;
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

  xy_avg = malloc(sizeof(int) * 2 * samples_avg);
  xy_small = malloc(sizeof(int) * 2 * samples_small);
  if(!xy_avg || !xy_small)
  {
    fprintf(stderr, "Error allocating memory for fibonacci lattice in: defringe module\n");
    goto ERROR_EXIT;
  }

  // precompute all required fibonacci lattices:
  for(int u = 0; u < samples_avg; u++)
  {
    int dx, dy;
    fib_latt(&dx, &dy, avg_radius, u, sampleidx_avg);
    xy_avg[2*u] = dx;
    xy_avg[2*u+1] = dy;
  }
  for(int u = 0; u < samples_small; u++)
  {
    int dx, dy;
    fib_latt(&dx, &dy, small_radius, u, sampleidx_small);
    xy_small[2*u] = dx;
    xy_small[2*u+1] = dy;
  }

  const float use_global_average = MODE_GLOBAL_AVERAGE == d->op_mode;
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(in, out, use_global_average) \
  dt_omp_sharedconst(width, height) \
  reduction(+ : avg_edge_chroma) \
  schedule(simd:static)
#endif
  for(size_t j = 0; j < (size_t)height * width * 4; j += 4)
  {
    // edge-detect on color channels
    // method: difference of original to gaussian blurred image:
    const float a = in[j + 1] - out[j + 1];
    const float b = in[j + 2] - out[j + 2];
    const float edge = (a * a + b * b); // range up to 2*(256)^2 -> approx. 0 to 131072

    // save local edge chroma in out[.. +3] , this is later compared with threshold
    out[j + 3] = edge;
    // the average chroma of the edge-layer in the roi
    avg_edge_chroma += edge * use_global_average;
  }

  float thresh;
  if(use_global_average)
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
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, in, out, samples_avg, samples_small) \
  dt_omp_sharedconst(d, width, height) \
  shared(xy_small, xy_avg) \
  firstprivate(thresh, avg_edge_chroma) \
  schedule(dynamic,3)
#endif
  for(int v = 0; v < height; v++)
  {
    const size_t row_above = (size_t)MAX(0, (v-1)) * width * ch;
    const size_t curr_row = (size_t)v * width * ch;
    const size_t row_below = (size_t)MIN((height-1), (v+1)) * width * ch;
    for(int t = 0; t < width; t++)
    {
      const size_t index = ch * ((size_t)v * width + t);
      float local_thresh = thresh;
      // think of compiler setting "-funswitch-loops" to maybe improve these things:
      if(MODE_LOCAL_AVERAGE == d->op_mode && out[index + 3] > thresh)
      {
        float local_avg = 0.0;
        // use some and not all values from the neighbourhood to speed things up:
        for(int u = 0; u < samples_avg; u++)
        {
          const int dx = xy_avg[2*u];
          const int dy = xy_avg[2*u+1];
          const int x = CLAMP(t + dx, 0, width - 1);
          const int y = CLAMP(v + dy, 0, height - 1);
          local_avg += out[((size_t)y * width + x) * ch + 3];
        }
        avg_edge_chroma = fmax(0.01f, (float)local_avg / samples_avg);
        local_thresh = fmax(0.1f, 4.0 * d->thresh * avg_edge_chroma / MAGIC_THRESHOLD_COEFF);
      }

      if(out[index + 3] > local_thresh
         // reduces artifacts ("region growing by 1 pixel"):
         || out[row_above + MAX(0, (t - 1)) * ch + 3] > local_thresh
         || out[row_above + t * ch + 3] > local_thresh
         || out[row_above + MIN(width - 1, (t + 1)) * ch + 3] > local_thresh
         || out[curr_row + MAX(0, (t - 1)) * ch + 3] > local_thresh
         || out[curr_row + MIN(width - 1, (t + 1)) * ch + 3] > local_thresh
         || out[row_below + MAX(0, (t - 1)) * ch + 3] > local_thresh
         || out[row_below + t * ch + 3] > local_thresh
         || out[row_below + MIN(width - 1, (t + 1)) * ch + 3] > local_thresh)
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
        for(int u = 0; u < samples_small; u++)
        {
          const int dx = xy_small[2*u];
          const int dy = xy_small[2*u+1];
          const int x = CLAMP(t + dx, 0, width - 1);
          const int y = CLAMP(v + dy, 0, height - 1);
          const size_t idx = ch * ((size_t)y * width + x);
          // inverse chroma weighted average of neighbouring pixels inside window
          // also taking average edge chromaticity into account (either global or local average)
          weight = 1.0 / (out[idx + 3] + avg_edge_chroma);
          atot += weight * in[idx + 1];
          btot += weight * in[idx + 2];
          norm += weight;
        }
        // here we could try using a "balance" between original and changed value, this could be used to
        // reduce artifacts
        // but on first tries, results weren't very convincing, and there are blend settings available anyway
        // in dt
        // float balance = (out[v*width*ch +t*ch +3]-thresh)/out[v*width*ch +t*ch +3];
        double a = (atot / norm); // *balance + in[v*width*ch + t*ch +1]*(1.0-balance);
        double b = (btot / norm); // *balance + in[v*width*ch + t*ch +2]*(1.0-balance);
        out[index] = in[index];
        out[index + 1] = a;
        out[index + 2] = b;
      }
      else
      {
        #ifdef _OPENMP
        #pragma omp simd aligned(in, out)
        #endif
        // we can't copy the alpha channel here because it contains info needed by neighboring pixels!
        for(int c = 0; c < 3; c++)
        {
          out[index+c] = in[index+c];
        }
      }
    }
  }

  goto FINISH_PROCESS;

ERROR_EXIT:
  dt_iop_image_copy_by_size(o, i, roi_out->width, roi_out->height, ch);

FINISH_PROCESS:
  free(xy_small);
  free(xy_avg);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_defringe_gui_data_t *g = IOP_GUI_ALLOC(defringe);

  g->mode_select = dt_bauhaus_combobox_from_params(self, "op_mode");
  gtk_widget_set_tooltip_text(g->mode_select,
      _("method for color protection:\n - global average: fast, might show slightly wrong previews in high "
        "magnification; might sometimes protect saturation too much or too low in comparison to local "
        "average\n - local average: slower, might protect saturation better than global average by using "
        "near pixels as color reference, so it can still allow for more desaturation where required\n - "
        "static: fast, only uses the threshold as a static limit"));

  g->radius_scale = dt_bauhaus_slider_from_params(self, "radius");
  gtk_widget_set_tooltip_text(g->radius_scale, _("radius for detecting fringe"));

  g->thresh_scale = dt_bauhaus_slider_from_params(self, "thresh");
  gtk_widget_set_tooltip_text(g->thresh_scale, _("threshold for defringe, higher values mean less defringing"));
}

void gui_update(dt_iop_module_t *module)
{
  dt_iop_defringe_gui_data_t *g = (dt_iop_defringe_gui_data_t *)module->gui_data;
  dt_iop_defringe_params_t *p = (dt_iop_defringe_params_t *)module->params;
  dt_bauhaus_combobox_set(g->mode_select, p->op_mode);
  dt_bauhaus_slider_set(g->radius_scale, p->radius);
  dt_bauhaus_slider_set(g->thresh_scale, p->thresh);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

