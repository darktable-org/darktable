/*
  This file is part of darktable,
  copyright (c) 2015 ulrich pegelow.

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
#include "bauhaus/bauhaus.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "control/control.h"
#include "common/debug.h"
#include "common/opencl.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE_INTROSPECTION(1, dt_iop_colorreconstruct_params_t)

typedef struct dt_iop_colorreconstruct_params_t
{
  float threshold;
  float spatial;
  float range;
} dt_iop_colorreconstruct_params_t;

typedef struct dt_iop_colorreconstruct_gui_data_t
{
  GtkWidget *threshold;
  GtkWidget *spatial;
  GtkWidget *range;
} dt_iop_colorreconstruct_gui_data_t;

typedef struct dt_iop_colorreconstruct_data_t
{
  float threshold;
  float spatial;
  float range;
} dt_iop_colorreconstruct_data_t;

typedef struct dt_iop_colorreconstruct_global_data_t
{
  int kernel_colorreconstruct;
} dt_iop_colorreconstruct_global_data_t;


const char *name()
{
  return _("color reconstruction");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int groups()
{
  return IOP_GROUP_BASIC;
}


void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "threshold"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "spatial blur"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "range blur"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_colorreconstruct_gui_data_t *g = (dt_iop_colorreconstruct_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "threshold", GTK_WIDGET(g->threshold));
  dt_accel_connect_slider_iop(self, "spatial blur", GTK_WIDGET(g->spatial));
  dt_accel_connect_slider_iop(self, "range blur", GTK_WIDGET(g->range));
}



#define DT_COMMON_BILATERAL_MAX_RES_S 6000
#define DT_COMMON_BILATERAL_MAX_RES_R 50

typedef struct dt_iop_colorreconstruct_cell_t
{
  float weight;
  float a;
  float b;
} dt_iop_colorreconstruct_cell_t;

typedef struct dt_iop_colorreconstruct_bilateral_t
{
  size_t size_x, size_y, size_z;
  int width, height;
  float sigma_s, sigma_r;
  dt_iop_colorreconstruct_cell_t *buf;
} dt_iop_colorreconstruct_bilateral_t;

static void image_to_grid(const dt_iop_colorreconstruct_bilateral_t *const b, const int i, const int j, const float L, float *x,
                          float *y, float *z)
{
  *x = CLAMPS(i / b->sigma_s, 0, b->size_x - 1);
  *y = CLAMPS(j / b->sigma_s, 0, b->size_y - 1);
  *z = CLAMPS(L / b->sigma_r, 0, b->size_z - 1);
}

static dt_iop_colorreconstruct_bilateral_t *dt_iop_colorreconstruct_bilateral_init(const int width,     // width of input image
                                                                                   const int height,    // height of input image
                                                                                   const float sigma_s, // spatial sigma (blur pixel coords)
                                                                                   const float sigma_r) // range sigma (blur luma values)
{
  dt_iop_colorreconstruct_bilateral_t *b = (dt_iop_colorreconstruct_bilateral_t *)malloc(sizeof(dt_iop_colorreconstruct_bilateral_t));
  if(!b) return NULL;
  float _x = roundf(width / sigma_s);
  float _y = roundf(height / sigma_s);
  float _z = roundf(100.0f / sigma_r);
  b->size_x = CLAMPS((int)_x, 4, DT_COMMON_BILATERAL_MAX_RES_S) + 1;
  b->size_y = CLAMPS((int)_y, 4, DT_COMMON_BILATERAL_MAX_RES_S) + 1;
  b->size_z = CLAMPS((int)_z, 4, DT_COMMON_BILATERAL_MAX_RES_R) + 1;
  b->width = width;
  b->height = height;
  b->sigma_s = MAX(height / (b->size_y - 1.0f), width / (b->size_x - 1.0f));
  b->sigma_r = 100.0f / (b->size_z - 1.0f);
  b->buf = dt_alloc_align(16, b->size_x * b->size_y * b->size_z * sizeof(dt_iop_colorreconstruct_cell_t));

  memset(b->buf, 0, b->size_x * b->size_y * b->size_z * sizeof(dt_iop_colorreconstruct_cell_t));
#if 0
  fprintf(stderr, "[bilateral] created grid [%d %d %d]"
          " with sigma (%f %f) (%f %f)\n", b->size_x, b->size_y, b->size_z,
          b->sigma_s, sigma_s, b->sigma_r, sigma_r);
#endif
  return b;
}

static void dt_iop_colorreconstruct_bilateral_splat(dt_iop_colorreconstruct_bilateral_t *b, const float *const in, const float threshold)
{
  const int ox = 1;
  const int oy = b->size_x;
  const int oz = b->size_y * b->size_x;
// splat into downsampled grid
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(b)
#endif
  for(int j = 0; j < b->height; j++)
  {
    size_t index = 4 * j * b->width;
    for(int i = 0; i < b->width; i++, index += 4)
    {
      float x, y, z;
      const float Lin = in[index];
      const float ain = in[index + 1];
      const float bin = in[index + 2];
      // we ignore overexposed areas as they lack color information
      if (Lin > threshold) continue;
      image_to_grid(b, i, j, Lin, &x, &y, &z);
      const int xi = MIN((int)x, b->size_x - 2);
      const int yi = MIN((int)y, b->size_y - 2);
      const int zi = MIN((int)z, b->size_z - 2);
      const float xf = x - xi;
      const float yf = y - yi;
      const float zf = z - zi;
      // nearest neighbour splatting:
      const size_t grid_index = xi + b->size_x * (yi + b->size_y * zi);
      // sum up payload here, doesn't have to be same as edge stopping data
      // for cross bilateral applications.
      // also note that this is not clipped (as L->z is), so potentially hdr/out of gamut
      // should not cause clipping here.
      for(int k = 0; k < 8; k++)
      {
        const size_t ii = grid_index + ((k & 1) ? ox : 0) + ((k & 2) ? oy : 0) + ((k & 4) ? oz : 0);
        const float contrib = ((k & 1) ? xf : (1.0f - xf)) * ((k & 2) ? yf : (1.0f - yf))
                              * ((k & 4) ? zf : (1.0f - zf)) * 100.0f / (b->sigma_s * b->sigma_s);
#ifdef _OPENMP
#pragma omp atomic
#endif
        b->buf[ii].weight += contrib;
#ifdef _OPENMP
#pragma omp atomic
#endif
        b->buf[ii].a += contrib*ain;
#ifdef _OPENMP
#pragma omp atomic
#endif
        b->buf[ii].b += contrib*bin;
      }
    }
  }
}


static void blur_line(dt_iop_colorreconstruct_cell_t *buf, const int offset1, const int offset2, const int offset3, const int size1,
                      const int size2, const int size3)
{
  const float w0 = 6.f / 16.f;
  const float w1 = 4.f / 16.f;
  const float w2 = 1.f / 16.f;
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(buf)
#endif
  for(int k = 0; k < size1; k++)
  {
    size_t index = (size_t)k * offset1;
    for(int j = 0; j < size2; j++)
    {
      dt_iop_colorreconstruct_cell_t tmp1 = buf[index];
      buf[index].weight = buf[index].weight * w0 + w1 * buf[index + offset3].weight + w2 * buf[index + 2 * offset3].weight;
      buf[index].a      = buf[index].a      * w0 + w1 * buf[index + offset3].a      + w2 * buf[index + 2 * offset3].a;
      buf[index].b      = buf[index].b      * w0 + w1 * buf[index + offset3].b      + w2 * buf[index + 2 * offset3].b;
      index += offset3;
      dt_iop_colorreconstruct_cell_t tmp2 = buf[index];
      buf[index].weight = buf[index].weight * w0 + w1 * (buf[index + offset3].weight + tmp1.weight) + w2 * buf[index + 2 * offset3].weight;
      buf[index].a      = buf[index].a      * w0 + w1 * (buf[index + offset3].a      + tmp1.a)      + w2 * buf[index + 2 * offset3].a;
      buf[index].b      = buf[index].b      * w0 + w1 * (buf[index + offset3].b      + tmp1.b)      + w2 * buf[index + 2 * offset3].b;
      index += offset3;
      for(int i = 2; i < size3 - 2; i++)
      {
        const dt_iop_colorreconstruct_cell_t tmp3 = buf[index];
        buf[index].weight = buf[index].weight * w0 + w1 * (buf[index + offset3].weight + tmp2.weight)
                     + w2 * (buf[index + 2 * offset3].weight + tmp1.weight);
        buf[index].a      = buf[index].a      * w0 + w1 * (buf[index + offset3].a      + tmp2.a)
                     + w2 * (buf[index + 2 * offset3].a      + tmp1.a);
        buf[index].b      = buf[index].b      * w0 + w1 * (buf[index + offset3].b      + tmp2.b)
                     + w2 * (buf[index + 2 * offset3].b      + tmp1.b);

        index += offset3;
        tmp1 = tmp2;
        tmp2 = tmp3;
      }
      const dt_iop_colorreconstruct_cell_t tmp3 = buf[index];
      buf[index].weight = buf[index].weight * w0 + w1 * (buf[index + offset3].weight + tmp2.weight) + w2 * tmp1.weight;
      buf[index].a      = buf[index].a      * w0 + w1 * (buf[index + offset3].a      + tmp2.a)      + w2 * tmp1.a;
      buf[index].b      = buf[index].b      * w0 + w1 * (buf[index + offset3].b      + tmp2.b)      + w2 * tmp1.b;
      index += offset3;
      buf[index].weight = buf[index].weight * w0 + w1 * tmp3.weight + w2 * tmp2.weight;
      buf[index].a      = buf[index].a      * w0 + w1 * tmp3.a      + w2 * tmp2.a;
      buf[index].b      = buf[index].b      * w0 + w1 * tmp3.b      + w2 * tmp2.b;
      index += offset3;
      index += offset2 - offset3 * size3;
    }
  }
}


static void dt_iop_colorreconstruct_bilateral_blur(dt_iop_colorreconstruct_bilateral_t *b)
{
  // gaussian up to 3 sigma
  blur_line(b->buf, b->size_x * b->size_y, b->size_x, 1, b->size_z, b->size_y, b->size_x);
  // gaussian up to 3 sigma
  blur_line(b->buf, b->size_x * b->size_y, 1, b->size_x, b->size_z, b->size_x, b->size_y);
  // gaussian up to 3 sigma
  blur_line(b->buf, 1, b->size_x, b->size_x * b->size_y, b->size_x, b->size_y, b->size_z);
}

static void dt_iop_colorreconstruct_bilateral_slice(const dt_iop_colorreconstruct_bilateral_t *const b, const float *const in, float *out,
                                                    const float threshold)
{
  const dt_iop_colorreconstruct_cell_t neutral = { 1.0f, 0.0f, 0.0f };
  const int ox = 1;
  const int oy = b->size_x;
  const int oz = b->size_y * b->size_x;
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out)
#endif
  for(int j = 0; j < b->height; j++)
  {
    size_t index = 4 * j * b->width;
    for(int i = 0; i < b->width; i++, index += 4)
    {
      float x, y, z;
      const float Lin = out[index + 0] = in[index + 0];
      const float ain = out[index + 1] = in[index + 1];
      const float bin = out[index + 2] = in[index + 2];
      out[index + 3] = in[index + 3];
      const float blend = CLAMPS(20.0f / threshold * Lin - 19.0f, 0.0f, 1.0f);
      if (blend == 0.0f) continue;
      image_to_grid(b, i, j, Lin, &x, &y, &z);
      // trilinear lookup:
      const int xi = MIN((int)x, b->size_x - 2);
      const int yi = MIN((int)y, b->size_y - 2);
      const int zi = MIN((int)z, b->size_z - 2);
      const float xf = x - xi;
      const float yf = y - yi;
      const float zf = z - zi;
      const size_t gi = xi + b->size_x * (yi + b->size_y * zi);

      dt_iop_colorreconstruct_cell_t ci   = (b->buf[gi]).weight > 0.0f ? b->buf[gi] : neutral;
      dt_iop_colorreconstruct_cell_t ciox = (b->buf[gi + ox]).weight > 0.0f ? b->buf[gi + ox] : neutral;
      dt_iop_colorreconstruct_cell_t cioy = (b->buf[gi + oy]).weight > 0.0f ? b->buf[gi + oy] : neutral;
      dt_iop_colorreconstruct_cell_t cioz = (b->buf[gi + oz]).weight > 0.0f ? b->buf[gi + oz] : neutral;

      dt_iop_colorreconstruct_cell_t cioxoy = (b->buf[gi + ox + oy]).weight > 0.0f ? b->buf[gi + ox + oy] : neutral;
      dt_iop_colorreconstruct_cell_t cioxoz = (b->buf[gi + ox + oz]).weight > 0.0f ? b->buf[gi + ox + oz] : neutral;
      dt_iop_colorreconstruct_cell_t cioyoz = (b->buf[gi + oy + oz]).weight > 0.0f ? b->buf[gi + oy + oz] : neutral;

      dt_iop_colorreconstruct_cell_t cioxoyoz = (b->buf[gi + ox + oy + oz]).weight > 0.0f ? b->buf[gi + ox + oy + oz] : neutral;

      const float aout =   ci.a/ci.weight * (1.0f - xf) * (1.0f - yf) * (1.0f - zf)
                         + ciox.a/ciox.weight * (xf) * (1.0f - yf) * (1.0f - zf)
                         + cioy.a/cioy.weight * (1.0f - xf) * (yf) * (1.0f - zf)
                         + cioxoy.a/cioxoy.weight * (xf) * (yf) * (1.0f - zf)
                         + cioz.a/cioz.weight * (1.0f - xf) * (1.0f - yf) * (zf)
                         + cioxoz.a/cioxoz.weight * (xf) * (1.0f - yf) * (zf)
                         + cioyoz.a/cioyoz.weight * (1.0f - xf) * (yf) * (zf)
                         + cioxoyoz.a/cioxoyoz.weight * (xf) * (yf) * (zf);


      const float bout =   ci.b/ci.weight * (1.0f - xf) * (1.0f - yf) * (1.0f - zf)
                         + ciox.b/ciox.weight * (xf) * (1.0f - yf) * (1.0f - zf)
                         + cioy.b/cioy.weight * (1.0f - xf) * (yf) * (1.0f - zf)
                         + cioxoy.b/cioxoy.weight * (xf) * (yf) * (1.0f - zf)
                         + cioz.b/cioz.weight * (1.0f - xf) * (1.0f - yf) * (zf)
                         + cioxoz.b/cioxoz.weight * (xf) * (1.0f - yf) * (zf)
                         + cioyoz.b/cioyoz.weight * (1.0f - xf) * (yf) * (zf)
                         + cioxoyoz.b/cioxoyoz.weight * (xf) * (yf) * (zf);

      out[index + 1] = ain * (1.0f - blend) + aout * blend;
      out[index + 2] = bin * (1.0f - blend) + bout * blend;
    }
  }
}

void dt_iop_colorreconstruct_bilateral_free(dt_iop_colorreconstruct_bilateral_t *b)
{
  if(!b) return;
  dt_free_align(b->buf);
  free(b);
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_colorreconstruct_data_t *data = (dt_iop_colorreconstruct_data_t *)piece->data;
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;

  const int width = roi_in->width;
  const int height = roi_in->height;

  const float scale = piece->iscale / roi_in->scale;
  const float sigma_r = data->range;
  const float sigma_s = data->spatial / scale;

  dt_iop_colorreconstruct_bilateral_t *b = dt_iop_colorreconstruct_bilateral_init(width, height, sigma_s, sigma_r);
  dt_iop_colorreconstruct_bilateral_splat(b, in, data->threshold);
  dt_iop_colorreconstruct_bilateral_blur(b);
  dt_iop_colorreconstruct_bilateral_slice(b, in, out, data->threshold);
  dt_iop_colorreconstruct_bilateral_free(b);
}


static void threshold_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorreconstruct_params_t *p = (dt_iop_colorreconstruct_params_t *)self->params;
  p->threshold = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void spatial_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorreconstruct_params_t *p = (dt_iop_colorreconstruct_params_t *)self->params;
  p->spatial = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void range_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorreconstruct_params_t *p = (dt_iop_colorreconstruct_params_t *)self->params;
  p->range = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorreconstruct_params_t *p = (dt_iop_colorreconstruct_params_t *)p1;
  dt_iop_colorreconstruct_data_t *d = (dt_iop_colorreconstruct_data_t *)piece->data;

  d->threshold = p->threshold;
  d->spatial = p->spatial;
  d->range = p->range;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorreconstruct_data_t *d = (dt_iop_colorreconstruct_data_t *)calloc(1, sizeof(dt_iop_colorreconstruct_data_t));
  piece->data = (void *)d;
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_colorreconstruct_gui_data_t *g = (dt_iop_colorreconstruct_gui_data_t *)self->gui_data;
  dt_iop_colorreconstruct_params_t *p = (dt_iop_colorreconstruct_params_t *)module->params;
  dt_bauhaus_slider_set(g->threshold, p->threshold);
  dt_bauhaus_slider_set(g->spatial, p->spatial);
  dt_bauhaus_slider_set(g->range, p->range);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_colorreconstruct_params_t));
  module->default_params = malloc(sizeof(dt_iop_colorreconstruct_params_t));
  module->default_enabled = 0;
  module->priority = 530; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_colorreconstruct_params_t);
  module->gui_data = NULL;
  dt_iop_colorreconstruct_params_t tmp = (dt_iop_colorreconstruct_params_t){ 100.0f, 200.0f, 10.0f };
  memcpy(module->params, &tmp, sizeof(dt_iop_colorreconstruct_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorreconstruct_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  dt_iop_colorreconstruct_global_data_t *gd
      = (dt_iop_colorreconstruct_global_data_t *)malloc(sizeof(dt_iop_colorreconstruct_global_data_t));
  module->data = gd;
  gd->kernel_colorreconstruct = -1;
}


void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorreconstruct_global_data_t *gd = (dt_iop_colorreconstruct_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorreconstruct);
  free(module->data);
  module->data = NULL;
}


void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_colorreconstruct_gui_data_t));
  dt_iop_colorreconstruct_gui_data_t *g = (dt_iop_colorreconstruct_gui_data_t *)self->gui_data;
  dt_iop_colorreconstruct_params_t *p = (dt_iop_colorreconstruct_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->threshold = dt_bauhaus_slider_new_with_range(self, 50.0f, 150.0f, 0.1f, p->threshold, 2);
  g->spatial = dt_bauhaus_slider_new_with_range(self, 0.0f, 1000.0f, 1.0f, p->spatial, 2);
  g->range = dt_bauhaus_slider_new_with_range(self, 0.0f, 50.0f, 1.0, p->range, 2);

  dt_bauhaus_widget_set_label(g->threshold, NULL, _("threshold"));
  dt_bauhaus_widget_set_label(g->spatial, NULL, _("spatial blur"));
  dt_bauhaus_widget_set_label(g->range, NULL, _("range blur"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->threshold, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->spatial, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->range, TRUE, TRUE, 0);

  g_object_set(g->threshold, "tooltip-text", _("pixels with L values below this threshold are not affected"), (char *)NULL);
  g_object_set(g->spatial, "tooltip-text", _("blur of color information in spatial dimensions (width and height)"), (char *)NULL);
  g_object_set(g->range, "tooltip-text", _("blur of color information in the luminance dimension (L value)"), (char *)NULL);

  g_signal_connect(G_OBJECT(g->threshold), "value-changed", G_CALLBACK(threshold_callback), self);
  g_signal_connect(G_OBJECT(g->spatial), "value-changed", G_CALLBACK(spatial_callback), self);
  g_signal_connect(G_OBJECT(g->range), "value-changed", G_CALLBACK(range_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
