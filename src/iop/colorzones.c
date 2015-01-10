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
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/opencl.h"
#include "develop/develop.h"
#include "control/control.h"
#include "control/conf.h"
#include "dtgtk/drawingarea.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "gui/presets.h"
#include "bauhaus/bauhaus.h"

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <inttypes.h>

DT_MODULE_INTROSPECTION(3, dt_iop_colorzones_params_t)

#define DT_IOP_COLORZONES_INSET DT_PIXEL_APPLY_DPI(5)
#define DT_IOP_COLORZONES_CURVE_INFL .3f
#define DT_IOP_COLORZONES_RES 64
#define DT_IOP_COLORZONES_LUT_RES 0x10000

#define DT_IOP_COLORZONES_BANDS 8
#define DT_IOP_COLORZONES1_BANDS 6

typedef enum dt_iop_colorzones_channel_t
{
  DT_IOP_COLORZONES_L = 0,
  DT_IOP_COLORZONES_C = 1,
  DT_IOP_COLORZONES_h = 2
} dt_iop_colorzones_channel_t;

typedef struct dt_iop_colorzones_params_t
{
  int32_t channel;
  float equalizer_x[3][DT_IOP_COLORZONES_BANDS], equalizer_y[3][DT_IOP_COLORZONES_BANDS];
  float strength;
} dt_iop_colorzones_params_t;

typedef struct dt_iop_colorzones_params2_t
{
  int32_t channel;
  float equalizer_x[3][DT_IOP_COLORZONES_BANDS], equalizer_y[3][DT_IOP_COLORZONES_BANDS];
} dt_iop_colorzones_params2_t;

typedef struct dt_iop_colorzones_params1_t
{
  int32_t channel;
  float equalizer_x[3][DT_IOP_COLORZONES1_BANDS], equalizer_y[3][DT_IOP_COLORZONES1_BANDS];
} dt_iop_colorzones_params1_t;

typedef struct dt_iop_colorzones_gui_data_t
{
  dt_draw_curve_t *minmax_curve; // curve for gui to draw
  GtkBox *hbox;
  GtkDrawingArea *area;
  GtkNotebook *channel_tabs;
  GtkWidget *select_by;
  GtkWidget *strength;
  double mouse_x, mouse_y, mouse_pick;
  float mouse_radius;
  dt_iop_colorzones_params_t drag_params;
  int dragging;
  int x_move;
  dt_iop_colorzones_channel_t channel;
  float draw_xs[DT_IOP_COLORZONES_RES], draw_ys[DT_IOP_COLORZONES_RES];
  float draw_min_xs[DT_IOP_COLORZONES_RES], draw_min_ys[DT_IOP_COLORZONES_RES];
  float draw_max_xs[DT_IOP_COLORZONES_RES], draw_max_ys[DT_IOP_COLORZONES_RES];
  float band_hist[DT_IOP_COLORZONES_BANDS];
  float band_max;
  cmsHPROFILE hsRGB;
  cmsHPROFILE hLab;
  cmsHTRANSFORM xform;
} dt_iop_colorzones_gui_data_t;

typedef struct dt_iop_colorzones_data_t
{
  dt_draw_curve_t *curve[3];
  dt_iop_colorzones_channel_t channel;
  float lut[4][DT_IOP_COLORZONES_LUT_RES];
} dt_iop_colorzones_data_t;

typedef struct dt_iop_colorzones_global_data_t
{
  int kernel_colorzones;
} dt_iop_colorzones_global_data_t;

const char *name()
{
  return _("color zones");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int groups()
{
  return IOP_GROUP_COLOR;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 3)
  {
    const dt_iop_colorzones_params1_t *old = old_params;
    dt_iop_colorzones_params_t *new = new_params;

    new->channel = old->channel;

    // keep first point

    for(int i = 0; i < 3; i++)
    {
      new->equalizer_x[i][0] = old->equalizer_x[i][0];
      new->equalizer_y[i][0] = old->equalizer_y[i][0];
    }

    for(int i = 0; i < 3; i++)
      for(int k = 0; k < 6; k++)
      {
        //  first+1 and last-1 are set to just after and before the first and last point
        if(k == 0)
          new->equalizer_x[i][k + 1] = old->equalizer_x[i][k] + 0.001;
        else if(k == 5)
          new->equalizer_x[i][k + 1] = old->equalizer_x[i][k] - 0.001;
        else
          new->equalizer_x[i][k + 1] = old->equalizer_x[i][k];
        new->equalizer_y[i][k + 1] = old->equalizer_y[i][k];
      }

    // keep last point

    for(int i = 0; i < 3; i++)
    {
      new->equalizer_x[i][7] = old->equalizer_x[i][5];
      new->equalizer_y[i][7] = old->equalizer_y[i][5];
    }
    new->strength = 0.0;
    return 0;
  }
  if(old_version == 2 && new_version == 3)
  {
    const dt_iop_colorzones_params2_t *old = old_params;
    dt_iop_colorzones_params_t *new = new_params;
    new->channel = old->channel;

    for(int b = 0; b < DT_IOP_COLORZONES_BANDS; b++)
      for(int c = 0; c < 3; c++)
      {
        new->equalizer_x[c][b] = old->equalizer_x[c][b];
        new->equalizer_y[c][b] = old->equalizer_y[c][b];
      }
    new->strength = 0.0;
    return 0;
  }
  return 1;
}

static float lookup(const float *lut, const float i)
{
  const int bin0 = MIN(0xffff, MAX(0, (int)(DT_IOP_COLORZONES_LUT_RES * i)));
  const int bin1 = MIN(0xffff, MAX(0, (int)(DT_IOP_COLORZONES_LUT_RES * i) + 1));
  const float f = DT_IOP_COLORZONES_LUT_RES * i - bin0;
  return lut[bin1] * f + lut[bin0] * (1. - f);
}

static float strength(float value, float strength)
{
  return value + (value - 0.5) * (strength / 100.0);
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_colorzones_data_t *d = (dt_iop_colorzones_data_t *)(piece->data);
  const int ch = piece->colors;
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(roi_in, roi_out, d, i, o)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    float *in = (float *)i + ch * k;
    float *out = (float *)o + ch * k;
    const float a = in[1], b = in[2];
    const float h = fmodf(atan2f(b, a) + 2.0 * M_PI, 2.0 * M_PI) / (2.0 * M_PI);
    const float C = sqrtf(b * b + a * a);
    float select = 0.0f;
    float blend = 0.0f;
    switch(d->channel)
    {
      case DT_IOP_COLORZONES_L:
        select = fminf(1.0, in[0] / 100.0);
        break;
      case DT_IOP_COLORZONES_C:
        select = fminf(1.0, C / 128.0);
        break;
      default:
      case DT_IOP_COLORZONES_h:
        select = h;
        blend = powf(1.0f - C / 128.0f, 2.0f);
        break;
    }
    const float Lm = (blend * .5f + (1.0f - blend) * lookup(d->lut[0], select)) - .5f;
    const float hm = (blend * .5f + (1.0f - blend) * lookup(d->lut[2], select)) - .5f;
    blend *= blend; // saturation isn't as prone to artifacts:
    // const float Cm = 2.0 * (blend*.5f + (1.0f-blend)*lookup(d->lut[1], select));
    const float Cm = 2.0 * lookup(d->lut[1], select);
    const float L = in[0] * powf(2.0f, 4.0f * Lm);
    out[0] = L;
    out[1] = cosf(2.0 * M_PI * (h + hm)) * Cm * C;
    out[2] = sinf(2.0 * M_PI * (h + hm)) * Cm * C;
    out[3] = in[3];
  }
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_colorzones_data_t *d = (dt_iop_colorzones_data_t *)piece->data;
  dt_iop_colorzones_global_data_t *gd = (dt_iop_colorzones_global_data_t *)self->data;
  cl_mem dev_L, dev_a, dev_b = NULL;
  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
  dev_L = dt_opencl_copy_host_to_device(devid, d->lut[0], 256, 256, sizeof(float));
  dev_a = dt_opencl_copy_host_to_device(devid, d->lut[1], 256, 256, sizeof(float));
  dev_b = dt_opencl_copy_host_to_device(devid, d->lut[2], 256, 256, sizeof(float));
  if(dev_L == NULL || dev_a == NULL || dev_b == NULL) goto error;

  dt_opencl_set_kernel_arg(devid, gd->kernel_colorzones, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorzones, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorzones, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorzones, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorzones, 4, sizeof(int), (void *)&d->channel);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorzones, 5, sizeof(cl_mem), (void *)&dev_L);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorzones, 6, sizeof(cl_mem), (void *)&dev_a);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorzones, 7, sizeof(cl_mem), (void *)&dev_b);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colorzones, sizes);

  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(dev_L);
  dt_opencl_release_mem_object(dev_a);
  dt_opencl_release_mem_object(dev_b);
  return TRUE;

error:
  if(dev_L != NULL) dt_opencl_release_mem_object(dev_L);
  if(dev_a != NULL) dt_opencl_release_mem_object(dev_a);
  if(dev_b != NULL) dt_opencl_release_mem_object(dev_b);
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorzones] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_colorzones_global_data_t *gd
      = (dt_iop_colorzones_global_data_t *)malloc(sizeof(dt_iop_colorzones_global_data_t));
  module->data = gd;
  gd->kernel_colorzones = dt_opencl_create_kernel(program, "colorzones");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorzones_global_data_t *gd = (dt_iop_colorzones_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorzones);
  free(module->data);
  module->data = NULL;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  // pull in new params to gegl
  dt_iop_colorzones_data_t *d = (dt_iop_colorzones_data_t *)(piece->data);
  dt_iop_colorzones_params_t *p = (dt_iop_colorzones_params_t *)p1;
#ifdef HAVE_GEGL
// TODO
#else
#if 0 // print new preset
  printf("p.channel = %d;\n", p->channel);
  for(int k=0; k<3; k++) for(int i=0; i<DT_IOP_COLORZONES_BANDS; i++)
    {
      printf("p.equalizer_x[%d][%i] = %f;\n", k, i, p->equalizer_x[k][i]);
      printf("p.equalizer_y[%d][%i] = %f;\n", k, i, p->equalizer_y[k][i]);
    }
#endif
  d->channel = (dt_iop_colorzones_channel_t)p->channel;
  for(int ch = 0; ch < 3; ch++)
  {
    if(d->channel == DT_IOP_COLORZONES_h)
      dt_draw_curve_set_point(d->curve[ch], 0, p->equalizer_x[ch][DT_IOP_COLORZONES_BANDS - 2] - 1.0,
                              strength(p->equalizer_y[ch][DT_IOP_COLORZONES_BANDS - 2], p->strength));
    else
      dt_draw_curve_set_point(d->curve[ch], 0, p->equalizer_x[ch][DT_IOP_COLORZONES_BANDS - 2] - 1.0,
                              strength(p->equalizer_y[ch][0], p->strength));
    for(int k = 0; k < DT_IOP_COLORZONES_BANDS; k++)
      dt_draw_curve_set_point(d->curve[ch], k + 1, p->equalizer_x[ch][k],
                              strength(p->equalizer_y[ch][k], p->strength));
    if(d->channel == DT_IOP_COLORZONES_h)
      dt_draw_curve_set_point(d->curve[ch], DT_IOP_COLORZONES_BANDS + 1, p->equalizer_x[ch][1] + 1.0,
                              strength(p->equalizer_y[ch][1], p->strength));
    else
      dt_draw_curve_set_point(d->curve[ch], DT_IOP_COLORZONES_BANDS + 1, p->equalizer_x[ch][1] + 1.0,
                              strength(p->equalizer_y[ch][DT_IOP_COLORZONES_BANDS - 1], p->strength));
    dt_draw_curve_calc_values(d->curve[ch], 0.0, 1.0, DT_IOP_COLORZONES_LUT_RES, d->lut[3], d->lut[ch]);
  }
#endif
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorzones_data_t *d = (dt_iop_colorzones_data_t *)malloc(sizeof(dt_iop_colorzones_data_t));
  dt_iop_colorzones_params_t *default_params = (dt_iop_colorzones_params_t *)self->default_params;
  piece->data = (void *)d;
  for(int ch = 0; ch < 3; ch++)
  {
    d->curve[ch] = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
    (void)dt_draw_curve_add_point(d->curve[ch],
                                  default_params->equalizer_x[ch][DT_IOP_COLORZONES_BANDS - 2] - 1.0,
                                  default_params->equalizer_y[ch][DT_IOP_COLORZONES_BANDS - 2]);
    for(int k = 0; k < DT_IOP_COLORZONES_BANDS; k++)
      (void)dt_draw_curve_add_point(d->curve[ch], default_params->equalizer_x[ch][k],
                                    default_params->equalizer_y[ch][k]);
    (void)dt_draw_curve_add_point(d->curve[ch], default_params->equalizer_x[ch][1] + 1.0,
                                  default_params->equalizer_y[ch][1]);
  }
  d->channel = (dt_iop_colorzones_channel_t)default_params->channel;
#ifdef HAVE_GEGL
#error "gegl version not implemented!"
#endif
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
// clean up everything again.
#ifdef HAVE_GEGL
#error "gegl version not implemented!"
#endif
  dt_iop_colorzones_data_t *d = (dt_iop_colorzones_data_t *)(piece->data);
  for(int ch = 0; ch < 3; ch++) dt_draw_curve_destroy(d->curve[ch]);
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_colorzones_gui_data_t *g = (dt_iop_colorzones_gui_data_t *)self->gui_data;
  dt_iop_colorzones_params_t *p = (dt_iop_colorzones_params_t *)self->params;
  dt_bauhaus_combobox_set(g->select_by, 2 - p->channel);
  dt_bauhaus_slider_set(g->strength, p->strength);
  gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_colorzones_params_t));
  module->default_params = malloc(sizeof(dt_iop_colorzones_params_t));
  module->default_enabled = 0; // we're a rather slow and rare op.
  module->priority = 583;      // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_colorzones_params_t);
  module->gui_data = NULL;
  dt_iop_colorzones_params_t tmp;
  for(int ch = 0; ch < 3; ch++)
  {
    for(int k = 0; k < DT_IOP_COLORZONES_BANDS; k++)
      tmp.equalizer_x[ch][k] = k / (DT_IOP_COLORZONES_BANDS - 1.0);
    for(int k = 0; k < DT_IOP_COLORZONES_BANDS; k++) tmp.equalizer_y[ch][k] = 0.5f;
  }
  tmp.strength = 0.0;
  tmp.channel = DT_IOP_COLORZONES_h;
  memcpy(module->params, &tmp, sizeof(dt_iop_colorzones_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorzones_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_colorzones_params_t p;

  p.strength = 0.0;

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "begin", NULL, NULL, NULL);

  // red black white

  p.channel = DT_IOP_COLORZONES_h;
  for(int k = 0; k < DT_IOP_COLORZONES_BANDS; k++)
  {
    p.equalizer_y[DT_IOP_COLORZONES_L][k] = .5f;
    p.equalizer_y[DT_IOP_COLORZONES_C][k] = .0f;
    p.equalizer_y[DT_IOP_COLORZONES_h][k] = .5f;
    p.equalizer_x[DT_IOP_COLORZONES_L][k] = k / (DT_IOP_COLORZONES_BANDS - 1.);
    p.equalizer_x[DT_IOP_COLORZONES_C][k] = k / (DT_IOP_COLORZONES_BANDS - 1.);
    p.equalizer_x[DT_IOP_COLORZONES_h][k] = k / (DT_IOP_COLORZONES_BANDS - 1.);
  }
  p.equalizer_y[DT_IOP_COLORZONES_C][0] = p.equalizer_y[DT_IOP_COLORZONES_C][DT_IOP_COLORZONES_BANDS - 1]
      = 0.65;
  p.equalizer_x[DT_IOP_COLORZONES_C][1] = 3. / 16.;
  p.equalizer_x[DT_IOP_COLORZONES_C][3] = 0.50;
  p.equalizer_x[DT_IOP_COLORZONES_C][4] = 0.51;
  p.equalizer_x[DT_IOP_COLORZONES_C][6] = 15. / 16.;
  dt_gui_presets_add_generic(_("red black white"), self->op, 3, &p, sizeof(p), 1);

  // black white and skin tones

  p.channel = DT_IOP_COLORZONES_h;
  for(int k = 0; k < DT_IOP_COLORZONES_BANDS; k++)
  {
    p.equalizer_y[DT_IOP_COLORZONES_L][k] = .5f;
    p.equalizer_y[DT_IOP_COLORZONES_C][k] = .0f;
    p.equalizer_y[DT_IOP_COLORZONES_h][k] = .5f;
    p.equalizer_x[DT_IOP_COLORZONES_L][k] = k / (DT_IOP_COLORZONES_BANDS - 1.);
    p.equalizer_x[DT_IOP_COLORZONES_C][k] = k / (DT_IOP_COLORZONES_BANDS - 1.);
    p.equalizer_x[DT_IOP_COLORZONES_h][k] = k / (DT_IOP_COLORZONES_BANDS - 1.);
  }
  p.equalizer_y[DT_IOP_COLORZONES_C][0] = p.equalizer_y[DT_IOP_COLORZONES_C][DT_IOP_COLORZONES_BANDS - 1]
      = 0.5;
  p.equalizer_x[DT_IOP_COLORZONES_C][2] = 0.25f;
  p.equalizer_x[DT_IOP_COLORZONES_C][1] = 0.16f;
  p.equalizer_y[DT_IOP_COLORZONES_C][1] = 0.3f;
  dt_gui_presets_add_generic(_("black white and skin tones"), self->op, 3, &p, sizeof(p), 1);

  // polarizing filter

  p.channel = DT_IOP_COLORZONES_C;
  for(int k = 0; k < DT_IOP_COLORZONES_BANDS; k++)
  {
    p.equalizer_y[DT_IOP_COLORZONES_L][k] = .5f;
    p.equalizer_y[DT_IOP_COLORZONES_C][k] = .5f;
    p.equalizer_y[DT_IOP_COLORZONES_h][k] = .5f;
    p.equalizer_x[DT_IOP_COLORZONES_L][k] = k / (DT_IOP_COLORZONES_BANDS - 1.);
    p.equalizer_x[DT_IOP_COLORZONES_C][k] = k / (DT_IOP_COLORZONES_BANDS - 1.);
    p.equalizer_x[DT_IOP_COLORZONES_h][k] = k / (DT_IOP_COLORZONES_BANDS - 1.);
  }
  for(int k = 3; k < DT_IOP_COLORZONES_BANDS; k++)
    p.equalizer_y[DT_IOP_COLORZONES_C][k] += (k - 2.5) / (DT_IOP_COLORZONES_BANDS - 2.0) * 0.25;
  for(int k = 4; k < DT_IOP_COLORZONES_BANDS; k++)
    p.equalizer_y[DT_IOP_COLORZONES_L][k] -= (k - 3.5) / (DT_IOP_COLORZONES_BANDS - 3.0) * 0.35;
  dt_gui_presets_add_generic(_("polarizing filter"), self->op, 3, &p, sizeof(p), 1);

  // natural skin tone

  p.channel = DT_IOP_COLORZONES_h;
  for(int k = 0; k < DT_IOP_COLORZONES_BANDS; k++)
  {
    p.equalizer_y[DT_IOP_COLORZONES_L][k] = .5f;
    p.equalizer_y[DT_IOP_COLORZONES_h][k] = .5f;
    p.equalizer_x[DT_IOP_COLORZONES_L][k] = k / (DT_IOP_COLORZONES_BANDS - 1.);
    p.equalizer_x[DT_IOP_COLORZONES_h][k] = k / (DT_IOP_COLORZONES_BANDS - 1.);
  }
  p.equalizer_x[DT_IOP_COLORZONES_C][0] = 0.000000;
  p.equalizer_y[DT_IOP_COLORZONES_C][0] = 0.468932;
  p.equalizer_x[DT_IOP_COLORZONES_C][1] = 0.010000;
  p.equalizer_y[DT_IOP_COLORZONES_C][1] = 0.468932;
  p.equalizer_x[DT_IOP_COLORZONES_C][2] = 0.120155;
  p.equalizer_y[DT_IOP_COLORZONES_C][2] = 0.445975;
  p.equalizer_x[DT_IOP_COLORZONES_C][3] = 0.248062;
  p.equalizer_y[DT_IOP_COLORZONES_C][3] = 0.468932;
  p.equalizer_x[DT_IOP_COLORZONES_C][4] = 0.500000;
  p.equalizer_y[DT_IOP_COLORZONES_C][4] = 0.499667;
  p.equalizer_x[DT_IOP_COLORZONES_C][5] = 0.748062;
  p.equalizer_y[DT_IOP_COLORZONES_C][5] = 0.500000;
  p.equalizer_x[DT_IOP_COLORZONES_C][6] = 0.990000;
  p.equalizer_y[DT_IOP_COLORZONES_C][6] = 0.468932;
  p.equalizer_x[DT_IOP_COLORZONES_C][7] = 1.000000;
  p.equalizer_y[DT_IOP_COLORZONES_C][7] = 0.468932;
  dt_gui_presets_add_generic(_("natural skin tones"), self->op, 3, &p, sizeof(p), 1);

  // black and white film

  p.channel = DT_IOP_COLORZONES_h;
  for(int k = 0; k < DT_IOP_COLORZONES_BANDS; k++)
  {
    p.equalizer_y[DT_IOP_COLORZONES_C][k] = .0f;
    p.equalizer_y[DT_IOP_COLORZONES_h][k] = .5f;
    p.equalizer_x[DT_IOP_COLORZONES_C][k] = k / (DT_IOP_COLORZONES_BANDS - 1.);
    p.equalizer_x[DT_IOP_COLORZONES_h][k] = k / (DT_IOP_COLORZONES_BANDS - 1.);
  }
  p.equalizer_x[DT_IOP_COLORZONES_L][0] = 0.000000;
  p.equalizer_y[DT_IOP_COLORZONES_L][0] = 0.613040;
  p.equalizer_x[DT_IOP_COLORZONES_L][1] = 0.010000;
  p.equalizer_y[DT_IOP_COLORZONES_L][1] = 0.613040;
  p.equalizer_x[DT_IOP_COLORZONES_L][2] = 0.245283;
  p.equalizer_y[DT_IOP_COLORZONES_L][2] = 0.447962;
  p.equalizer_x[DT_IOP_COLORZONES_L][3] = 0.498113;
  p.equalizer_y[DT_IOP_COLORZONES_L][3] = 0.529201;
  p.equalizer_x[DT_IOP_COLORZONES_L][4] = 0.641509;
  p.equalizer_y[DT_IOP_COLORZONES_L][4] = 0.664967;
  p.equalizer_x[DT_IOP_COLORZONES_L][5] = 0.879245;
  p.equalizer_y[DT_IOP_COLORZONES_L][5] = 0.777294;
  p.equalizer_x[DT_IOP_COLORZONES_L][6] = 0.990000;
  p.equalizer_y[DT_IOP_COLORZONES_L][6] = 0.613040;
  p.equalizer_x[DT_IOP_COLORZONES_L][7] = 1.000000;
  p.equalizer_y[DT_IOP_COLORZONES_L][7] = 0.613040;
  dt_gui_presets_add_generic(_("black & white film"), self->op, 3, &p, sizeof(p), 1);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "commit", NULL, NULL, NULL);
}

// fills in new parameters based on mouse position (in 0,1)
static void dt_iop_colorzones_get_params(dt_iop_colorzones_params_t *p, const int ch, const double mouse_x,
                                         const double mouse_y, const float rad)
{
  if(p->channel == DT_IOP_COLORZONES_h)
  {
    // periodic boundary
    for(int k = 1; k < DT_IOP_COLORZONES_BANDS - 1; k++)
    {
      const float f
          = expf(-(mouse_x - p->equalizer_x[ch][k]) * (mouse_x - p->equalizer_x[ch][k]) / (rad * rad));
      p->equalizer_y[ch][k] = (1 - f) * p->equalizer_y[ch][k] + f * mouse_y;
    }
    const int m = DT_IOP_COLORZONES_BANDS - 1;
    const float mind = fminf((mouse_x - p->equalizer_x[ch][0]) * (mouse_x - p->equalizer_x[ch][0]),
                             (mouse_x - p->equalizer_x[ch][m]) * (mouse_x - p->equalizer_x[ch][m]));
    const float f = expf(-mind / (rad * rad));
    p->equalizer_y[ch][0] = (1 - f) * p->equalizer_y[ch][0] + f * mouse_y;
    p->equalizer_y[ch][m] = (1 - f) * p->equalizer_y[ch][m] + f * mouse_y;
  }
  else
  {
    for(int k = 0; k < DT_IOP_COLORZONES_BANDS; k++)
    {
      const float f
          = expf(-(mouse_x - p->equalizer_x[ch][k]) * (mouse_x - p->equalizer_x[ch][k]) / (rad * rad));
      p->equalizer_y[ch][k] = (1 - f) * p->equalizer_y[ch][k] + f * mouse_y;
    }
  }
}

static gboolean colorzones_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorzones_gui_data_t *c = (dt_iop_colorzones_gui_data_t *)self->gui_data;
  dt_iop_colorzones_params_t p = *(dt_iop_colorzones_params_t *)self->params;
  int ch = (int)c->channel;
  if(p.channel == DT_IOP_COLORZONES_h)
    dt_draw_curve_set_point(c->minmax_curve, 0, p.equalizer_x[ch][DT_IOP_COLORZONES_BANDS - 2] - 1.0,
                            p.equalizer_y[ch][DT_IOP_COLORZONES_BANDS - 2]);
  else
    dt_draw_curve_set_point(c->minmax_curve, 0, p.equalizer_x[ch][DT_IOP_COLORZONES_BANDS - 2] - 1.0,
                            p.equalizer_y[ch][0]);
  for(int k = 0; k < DT_IOP_COLORZONES_BANDS; k++)
    dt_draw_curve_set_point(c->minmax_curve, k + 1, p.equalizer_x[ch][k], p.equalizer_y[ch][k]);
  if(p.channel == DT_IOP_COLORZONES_h)
    dt_draw_curve_set_point(c->minmax_curve, DT_IOP_COLORZONES_BANDS + 1, p.equalizer_x[ch][1] + 1.0,
                            p.equalizer_y[ch][1]);
  else
    dt_draw_curve_set_point(c->minmax_curve, DT_IOP_COLORZONES_BANDS + 1, p.equalizer_x[ch][1] + 1.0,
                            p.equalizer_y[ch][DT_IOP_COLORZONES_BANDS - 1]);

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int inset = DT_IOP_COLORZONES_INSET;
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg, match color of the notebook tabs:
  GdkRGBA color;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gboolean color_found = gtk_style_context_lookup_color (context, "selected_bg_color", &color);
  if(!color_found)
  {
    color.red = 1.0;
    color.green = 0.0;
    color.blue = 0.0;
    color.alpha = 1.0;
  }
  gdk_cairo_set_source_rgba(cr, &color);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2 * inset;
  height -= 2 * inset;

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
  cairo_set_source_rgb(cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);

  cairo_set_source_rgb(cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max curves:
    dt_iop_colorzones_get_params(&p, c->channel, c->mouse_x, 1., c->mouse_radius);
    if(p.channel == DT_IOP_COLORZONES_h)
      dt_draw_curve_set_point(c->minmax_curve, 0, p.equalizer_x[ch][DT_IOP_COLORZONES_BANDS - 2] - 1.0,
                              p.equalizer_y[ch][DT_IOP_COLORZONES_BANDS - 2]);
    else
      dt_draw_curve_set_point(c->minmax_curve, 0, p.equalizer_x[ch][DT_IOP_COLORZONES_BANDS - 2] - 1.0,
                              p.equalizer_y[ch][0]);
    for(int k = 0; k < DT_IOP_COLORZONES_BANDS; k++)
      dt_draw_curve_set_point(c->minmax_curve, k + 1, p.equalizer_x[ch][k], p.equalizer_y[ch][k]);
    if(p.channel == DT_IOP_COLORZONES_h)
      dt_draw_curve_set_point(c->minmax_curve, DT_IOP_COLORZONES_BANDS + 1, p.equalizer_x[ch][1] + 1.0,
                              p.equalizer_y[ch][1]);
    else
      dt_draw_curve_set_point(c->minmax_curve, DT_IOP_COLORZONES_BANDS + 1, p.equalizer_x[ch][1] + 1.0,
                              p.equalizer_y[ch][DT_IOP_COLORZONES_BANDS - 1]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, DT_IOP_COLORZONES_RES, c->draw_min_xs,
                              c->draw_min_ys);

    p = *(dt_iop_colorzones_params_t *)self->params;
    dt_iop_colorzones_get_params(&p, c->channel, c->mouse_x, .0, c->mouse_radius);
    if(p.channel == DT_IOP_COLORZONES_h)
      dt_draw_curve_set_point(c->minmax_curve, 0, p.equalizer_x[ch][DT_IOP_COLORZONES_BANDS - 2] - 1.0,
                              p.equalizer_y[ch][DT_IOP_COLORZONES_BANDS - 2]);
    else
      dt_draw_curve_set_point(c->minmax_curve, 0, p.equalizer_x[ch][DT_IOP_COLORZONES_BANDS - 2] - 1.0,
                              p.equalizer_y[ch][0]);
    for(int k = 0; k < DT_IOP_COLORZONES_BANDS; k++)
      dt_draw_curve_set_point(c->minmax_curve, k + 1, p.equalizer_x[ch][k], p.equalizer_y[ch][k]);
    if(p.channel == DT_IOP_COLORZONES_h)
      dt_draw_curve_set_point(c->minmax_curve, DT_IOP_COLORZONES_BANDS + 1, p.equalizer_x[ch][1] + 1.0,
                              p.equalizer_y[ch][1]);
    else
      dt_draw_curve_set_point(c->minmax_curve, DT_IOP_COLORZONES_BANDS + 1, p.equalizer_x[ch][1] + 1.0,
                              p.equalizer_y[ch][DT_IOP_COLORZONES_BANDS - 1]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, DT_IOP_COLORZONES_RES, c->draw_max_xs,
                              c->draw_max_ys);
  }

  if(self->picked_color_max[0] < 0.0f || self->picked_color[0] == 0.0f)
  {
    self->picked_color[0] = 50.0f;
    self->picked_color[1] = 0.0f;
    self->picked_color[2] = -5.0f;
  }

  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);

  const float pickC
      = sqrtf(self->picked_color[1] * self->picked_color[1] + self->picked_color[2] * self->picked_color[2]);
  const int cellsi = 16, cellsj = 9;
  for(int j = 0; j < cellsj; j++)
    for(int i = 0; i < cellsi; i++)
    {
      double rgb[3] = { 0.5, 0.5, 0.5 };
      float jj = 1.0 - (j - .5) / (cellsj - 1.), ii = (i + .5) / (cellsi - 1.);
      cmsCIELab Lab;
      switch(p.channel)
      {
        // select by channel, abscissa:
        case DT_IOP_COLORZONES_L:
          Lab.L = ii * 100.0;
          Lab.a = self->picked_color[1];
          Lab.b = self->picked_color[2];
          break;
        case DT_IOP_COLORZONES_C:
          Lab.L = 50.0;
          Lab.a = 64.0 * ii * self->picked_color[1] / pickC;
          Lab.b = 64.0 * ii * self->picked_color[2] / pickC;
          break;
        default: // case DT_IOP_COLORZONES_h:
          Lab.L = 50.0;
          Lab.a = cosf(2.0 * M_PI * ii) * 64.0f;
          Lab.b = sinf(2.0 * M_PI * ii) * 64.0f;
          break;
      }
      const float L0 = Lab.L;
      const float angle = atan2f(Lab.b, Lab.a);
      switch(c->channel)
      {
        // channel to be altered:
        case DT_IOP_COLORZONES_L:
          Lab.L += -50.0 + 100.0 * jj;
          break;
        case DT_IOP_COLORZONES_C:
          Lab.a *= 2.0f * jj;
          Lab.b *= 2.0f * jj;
          break;
        default: // DT_IOP_COLORZONES_h
          Lab.a = cosf(angle + 2.0 * M_PI * (jj - .5f)) * 64.0;
          Lab.b = sinf(angle + 2.0 * M_PI * (jj - .5f)) * 64.0;
          if(p.channel == DT_IOP_COLORZONES_C)
          {
            Lab.a *= ii;
            Lab.b *= ii;
          }
          break;
      }
      // gamut mapping magic from iop/exposure.c:
      const float Lwhite = 100.0f, Lclip = 20.0f;
      const float Lcap = fminf(100.0, Lab.L);
      const float clip = 1.0
                         - (Lcap - L0) * (1.0 / 100.0) * fminf(Lwhite - Lclip, fmaxf(0.0, Lab.L - Lclip))
                           / (Lwhite - Lclip);
      const float clip2 = clip * clip * clip;
      Lab.a *= Lab.L / L0 * clip2;
      Lab.b *= Lab.L / L0 * clip2;
      cmsDoTransform(c->xform, &Lab, rgb, 1);
      cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
      cairo_rectangle(cr, width * i / (float)cellsi, height * j / (float)cellsj,
                      width / (float)cellsi - DT_PIXEL_APPLY_DPI(1),
                      height / (float)cellsj - DT_PIXEL_APPLY_DPI(1));
      cairo_fill(cr);
    }

  cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);

  if(self->picked_color_max[0] >= 0.0f)
  {
    // draw marker for currently selected color:
    float picked_i = -1.0;
    switch(p.channel)
    {
      // select by channel, abscissa:
      case DT_IOP_COLORZONES_L:
        picked_i = self->picked_color[0] / 100.0;
        break;
      case DT_IOP_COLORZONES_C:
        picked_i = pickC / 128.0;
        break;
      default: // case DT_IOP_COLORZONES_h:
        picked_i = fmodf(atan2f(self->picked_color[2], self->picked_color[1]) + 2.0 * M_PI, 2.0 * M_PI)
                   / (2.0 * M_PI);
        break;
    }
    cairo_save(cr);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_set_operator(cr, CAIRO_OPERATOR_XOR);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
    cairo_move_to(cr, width * picked_i, 0.0);
    cairo_line_to(cr, width * picked_i, height);
    cairo_stroke(cr);
    cairo_restore(cr);
  }

  // draw x positions
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  const float arrw = DT_PIXEL_APPLY_DPI(7.0f);
  for(int k = 0; k < DT_IOP_COLORZONES_BANDS; k++)
  {
    cairo_move_to(cr, width * p.equalizer_x[c->channel][k], height + inset - DT_PIXEL_APPLY_DPI(1));
    cairo_rel_line_to(cr, -arrw * .5f, 0);
    cairo_rel_line_to(cr, arrw * .5f, -arrw);
    cairo_rel_line_to(cr, arrw * .5f, arrw);
    cairo_close_path(cr);
    if(c->x_move == k)
      cairo_fill(cr);
    else
      cairo_stroke(cr);
  }

  // draw selected cursor
  cairo_translate(cr, 0, height);

  // cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  for(int i = 0; i < 3; i++)
  {
    // draw curves, selected last.
    int ch = ((int)c->channel + i + 1) % 3;
    if(i == 2)
      cairo_set_source_rgba(cr, .7, .7, .7, 1.0);
    else
      cairo_set_source_rgba(cr, .7, .7, .7, 0.3);
    p = *(dt_iop_colorzones_params_t *)self->params;
    if(p.channel == DT_IOP_COLORZONES_h)
      dt_draw_curve_set_point(c->minmax_curve, 0, p.equalizer_x[ch][DT_IOP_COLORZONES_BANDS - 2] - 1.0,
                              p.equalizer_y[ch][DT_IOP_COLORZONES_BANDS - 2]);
    else
      dt_draw_curve_set_point(c->minmax_curve, 0, p.equalizer_x[ch][DT_IOP_COLORZONES_BANDS - 2] - 1.0,
                              p.equalizer_y[ch][0]);
    for(int k = 0; k < DT_IOP_COLORZONES_BANDS; k++)
      dt_draw_curve_set_point(c->minmax_curve, k + 1, p.equalizer_x[ch][k], p.equalizer_y[ch][k]);
    if(p.channel == DT_IOP_COLORZONES_h)
      dt_draw_curve_set_point(c->minmax_curve, DT_IOP_COLORZONES_BANDS + 1, p.equalizer_x[ch][1] + 1.0,
                              p.equalizer_y[ch][1]);
    else
      dt_draw_curve_set_point(c->minmax_curve, DT_IOP_COLORZONES_BANDS + 1, p.equalizer_x[ch][1] + 1.0,
                              p.equalizer_y[ch][DT_IOP_COLORZONES_BANDS - 1]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, DT_IOP_COLORZONES_RES, c->draw_xs, c->draw_ys);
    cairo_move_to(cr, 0 * width / (float)(DT_IOP_COLORZONES_RES - 1), -height * c->draw_ys[0]);
    for(int k = 1; k < DT_IOP_COLORZONES_RES; k++)
      cairo_line_to(cr, k * width / (float)(DT_IOP_COLORZONES_RES - 1), -height * c->draw_ys[k]);
    cairo_stroke(cr);
  }

  // draw dots on knots
  cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  for(int k = 0; k < DT_IOP_COLORZONES_BANDS; k++)
  {
    cairo_arc(cr, width * p.equalizer_x[c->channel][k], -height * p.equalizer_y[c->channel][k],
              DT_PIXEL_APPLY_DPI(3.0), 0.0, 2.0 * M_PI);
    if(c->x_move == k)
      cairo_fill(cr);
    else
      cairo_stroke(cr);
  }

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max, if selected
    cairo_set_source_rgba(cr, .7, .7, .7, .6);
    cairo_move_to(cr, 0, -height * c->draw_min_ys[0]);
    for(int k = 1; k < DT_IOP_COLORZONES_RES; k++)
      cairo_line_to(cr, k * width / (float)(DT_IOP_COLORZONES_RES - 1), -height * c->draw_min_ys[k]);
    for(int k = DT_IOP_COLORZONES_RES - 1; k >= 0; k--)
      cairo_line_to(cr, k * width / (float)(DT_IOP_COLORZONES_RES - 1), -height * c->draw_max_ys[k]);
    cairo_close_path(cr);
    cairo_fill(cr);
    // draw mouse focus circle
    cairo_set_source_rgba(cr, .9, .9, .9, .5);
    const float pos = DT_IOP_COLORZONES_RES * c->mouse_x;
    int k = (int)pos;
    const float f = k - pos;
    if(k >= DT_IOP_COLORZONES_RES - 1) k = DT_IOP_COLORZONES_RES - 2;
    float ht = -height * (f * c->draw_ys[k] + (1 - f) * c->draw_ys[k + 1]);
    cairo_arc(cr, c->mouse_x * width, ht, c->mouse_radius * width, 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean colorzones_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorzones_gui_data_t *c = (dt_iop_colorzones_gui_data_t *)self->gui_data;
  dt_iop_colorzones_params_t *p = (dt_iop_colorzones_params_t *)self->params;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int inset = DT_IOP_COLORZONES_INSET;
  int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
  if(!c->dragging) c->mouse_x = CLAMP(event->x - inset, 0, width) / (float)width;
  c->mouse_y = 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;
  if(c->dragging)
  {
    *p = c->drag_params;
    if(c->x_move >= 0)
    {
      const float mx = CLAMP(event->x - inset, 0, width) / (float)width;
      if(c->x_move > 0 && c->x_move < DT_IOP_COLORZONES_BANDS - 1)
      {
        const float minx = p->equalizer_x[c->channel][c->x_move - 1] + 0.001f;
        const float maxx = p->equalizer_x[c->channel][c->x_move + 1] - 0.001f;
        p->equalizer_x[c->channel][c->x_move] = fminf(maxx, fmaxf(minx, mx));
      }
    }
    else
    {
      dt_iop_colorzones_get_params(p, c->channel, c->mouse_x, c->mouse_y + c->mouse_pick, c->mouse_radius);
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else if(event->y > height)
  {
    c->x_move = 0;
    float dist = fabs(p->equalizer_x[c->channel][0] - c->mouse_x);
    for(int k = 1; k < DT_IOP_COLORZONES_BANDS; k++)
    {
      float d2 = fabs(p->equalizer_x[c->channel][k] - c->mouse_x);
      if(d2 < dist)
      {
        c->x_move = k;
        dist = d2;
      }
    }
  }
  else
  {
    c->x_move = -1;
  }
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean colorzones_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorzones_gui_data_t *c = (dt_iop_colorzones_gui_data_t *)self->gui_data;
  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    // reset current curve
    dt_iop_colorzones_params_t *p = (dt_iop_colorzones_params_t *)self->params;
    dt_iop_colorzones_params_t *d = (dt_iop_colorzones_params_t *)self->default_params;
    dt_iop_colorzones_gui_data_t *c = (dt_iop_colorzones_gui_data_t *)self->gui_data;
    for(int k = 0; k < DT_IOP_COLORZONES_BANDS; k++)
    {
      p->equalizer_x[c->channel][k] = d->equalizer_x[c->channel][k];
      p->equalizer_y[c->channel][k] = d->equalizer_y[c->channel][k];
    }
    p->strength = d->strength;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(self->widget);
  }
  else if(event->button == 1)
  {
    c->drag_params = *(dt_iop_colorzones_params_t *)self->params;

    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    const int inset = DT_IOP_COLORZONES_INSET;
    int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
    c->mouse_pick
        = dt_draw_curve_calc_value(c->minmax_curve, CLAMP(event->x - inset, 0, width) / (float)width);
    c->mouse_pick -= 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;
    c->dragging = 1;
    return TRUE;
  }
  return FALSE;
}

static gboolean colorzones_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_colorzones_gui_data_t *c = (dt_iop_colorzones_gui_data_t *)self->gui_data;
    c->dragging = 0;
    return TRUE;
  }
  return FALSE;
}

static gboolean colorzones_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorzones_gui_data_t *c = (dt_iop_colorzones_gui_data_t *)self->gui_data;
  c->mouse_y = fabs(c->mouse_y);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean colorzones_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorzones_gui_data_t *c = (dt_iop_colorzones_gui_data_t *)self->gui_data;
  // for fluxbox
  c->mouse_y = -fabs(c->mouse_y);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean colorzones_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorzones_gui_data_t *c = (dt_iop_colorzones_gui_data_t *)self->gui_data;
  if(event->direction == GDK_SCROLL_UP && c->mouse_radius > 0.2 / DT_IOP_COLORZONES_BANDS)
    c->mouse_radius *= 0.9; // 0.7;
  if(event->direction == GDK_SCROLL_DOWN && c->mouse_radius < 1.0) c->mouse_radius *= (1.0 / 0.9); // 1.42;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static void colorzones_tab_switch(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorzones_gui_data_t *c = (dt_iop_colorzones_gui_data_t *)self->gui_data;
  c->channel = (dt_iop_colorzones_channel_t)page_num;
  gtk_widget_queue_draw(self->widget);
}

static void select_by_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorzones_params_t *p = (dt_iop_colorzones_params_t *)self->params;
  memcpy(p, self->default_params, sizeof(dt_iop_colorzones_params_t));
  p->channel = 2 - (dt_iop_colorzones_channel_t)dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void request_pick_toggled(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  self->request_color_pick
      = (gtk_toggle_button_get_active(togglebutton) ? DT_REQUEST_COLORPICK_MODULE : DT_REQUEST_COLORPICK_OFF);

  /* set the area sample size */
  if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF)
  {
    dt_lib_colorpicker_set_point(darktable.lib, 0.5, 0.5);
    dt_dev_reprocess_all(self->dev);
  }
  else
    dt_control_queue_redraw();

  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  dt_iop_request_focus(self);
}

static void strength_changed(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorzones_params_t *p = (dt_iop_colorzones_params_t *)self->params;
  p->strength = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_colorzones_gui_data_t));
  dt_iop_colorzones_gui_data_t *c = (dt_iop_colorzones_gui_data_t *)self->gui_data;
  dt_iop_colorzones_params_t *p = (dt_iop_colorzones_params_t *)self->params;

  //   c->channel = DT_IOP_COLORZONES_C;
  c->channel = dt_conf_get_int("plugins/darkroom/colorzones/gui_channel");
  int ch = (int)c->channel;
  c->minmax_curve = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
  (void)dt_draw_curve_add_point(c->minmax_curve, p->equalizer_x[ch][DT_IOP_COLORZONES_BANDS - 2] - 1.0,
                                p->equalizer_y[ch][DT_IOP_COLORZONES_BANDS - 2]);
  for(int k = 0; k < DT_IOP_COLORZONES_BANDS; k++)
    (void)dt_draw_curve_add_point(c->minmax_curve, p->equalizer_x[ch][k], p->equalizer_y[ch][k]);
  (void)dt_draw_curve_add_point(c->minmax_curve, p->equalizer_x[ch][1] + 1.0, p->equalizer_y[ch][1]);
  c->mouse_x = c->mouse_y = c->mouse_pick = -1.0;
  c->dragging = 0;
  c->x_move = -1;
  c->mouse_radius = 1.0 / DT_IOP_COLORZONES_BANDS;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // tabs
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

  c->channel_tabs = GTK_NOTEBOOK(gtk_notebook_new());

  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs),
                           GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)),
                           gtk_label_new(_("lightness")));
  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs),
                           GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)),
                           gtk_label_new(_("saturation")));
  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs),
                           GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)), gtk_label_new(_("hue")));

  gtk_widget_show_all(GTK_WIDGET(gtk_notebook_get_nth_page(c->channel_tabs, c->channel)));
  gtk_notebook_set_current_page(GTK_NOTEBOOK(c->channel_tabs), c->channel);

  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(c->channel_tabs), FALSE, FALSE, 0);
  GtkWidget *tb = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT  | CPF_DO_NOT_USE_BORDER);
  gtk_widget_set_size_request(GTK_WIDGET(tb), DT_PIXEL_APPLY_DPI(14), DT_PIXEL_APPLY_DPI(14));
  g_object_set(G_OBJECT(tb), "tooltip-text", _("pick GUI color from image"), (char *)NULL);
  g_signal_connect(G_OBJECT(tb), "toggled", G_CALLBACK(request_pick_toggled), self);
  gtk_box_pack_end(GTK_BOX(hbox), tb, FALSE, FALSE, 0);


  g_signal_connect(G_OBJECT(c->channel_tabs), "switch_page", G_CALLBACK(colorzones_tab_switch), self);

  // the nice graph
  c->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(9.0 / 16.0));
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(c->area), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(vbox), TRUE, TRUE, 5);

  c->strength = dt_bauhaus_slider_new_with_range(self, -200, 200.0, 10.0, p->strength, 1);
  dt_bauhaus_slider_set_format(c->strength, "%.01f%%");
  dt_bauhaus_widget_set_label(c->strength, NULL, _("mix"));
  gtk_box_pack_start(GTK_BOX(self->widget), c->strength, TRUE, TRUE, 0);
  g_object_set(G_OBJECT(c->strength), "tooltip-text", _("make effect stronger or weaker"), (char *)NULL);
  g_signal_connect(G_OBJECT(c->strength), "value-changed", G_CALLBACK(strength_changed), (gpointer)self);

  // select by which dimension
  c->select_by = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(c->select_by, NULL, _("select by"));
  g_object_set(G_OBJECT(c->select_by), "tooltip-text",
               _("choose selection criterion, will be the abscissa in the graph"), (char *)NULL);
  dt_bauhaus_combobox_add(c->select_by, _("hue"));
  dt_bauhaus_combobox_add(c->select_by, _("saturation"));
  dt_bauhaus_combobox_add(c->select_by, _("lightness"));
  gtk_box_pack_start(GTK_BOX(self->widget), c->select_by, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(c->select_by), "value-changed", G_CALLBACK(select_by_changed), (gpointer)self);


  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                             | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                             | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK);
  g_signal_connect(G_OBJECT(c->area), "draw", G_CALLBACK(colorzones_draw), self);
  g_signal_connect(G_OBJECT(c->area), "button-press-event", G_CALLBACK(colorzones_button_press), self);
  g_signal_connect(G_OBJECT(c->area), "button-release-event", G_CALLBACK(colorzones_button_release), self);
  g_signal_connect(G_OBJECT(c->area), "motion-notify-event", G_CALLBACK(colorzones_motion_notify), self);
  g_signal_connect(G_OBJECT(c->area), "leave-notify-event", G_CALLBACK(colorzones_leave_notify), self);
  g_signal_connect(G_OBJECT(c->area), "enter-notify-event", G_CALLBACK(colorzones_enter_notify), self);
  g_signal_connect(G_OBJECT(c->area), "scroll-event", G_CALLBACK(colorzones_scrolled), self);


  c->hsRGB = dt_colorspaces_create_srgb_profile();
  c->hLab = dt_colorspaces_create_lab_profile();
  c->xform = cmsCreateTransform(c->hLab, TYPE_Lab_DBL, c->hsRGB, TYPE_RGB_DBL, INTENT_PERCEPTUAL, 0);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colorzones_gui_data_t *c = (dt_iop_colorzones_gui_data_t *)self->gui_data;
  dt_conf_set_int("plugins/darkroom/colorzones/gui_channel", c->channel);
  dt_colorspaces_cleanup_profile(c->hsRGB);
  dt_colorspaces_cleanup_profile(c->hLab);
  cmsDeleteTransform(c->xform);
  dt_draw_curve_destroy(c->minmax_curve);
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
