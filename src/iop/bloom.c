/*
    This file is part of darktable,
    copyright (c) 2010-2012 Henrik Andersson.

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
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"

#include <assert.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BOX_ITERATIONS 8
#define NUM_BUCKETS 4 /* OpenCL bucket chain size for tmp buffers; minimum 2 */

#define CLIP(x) ((x < 0) ? 0.0 : (x > 1.0) ? 1.0 : x)
#define LCLIP(x) ((x < 0) ? 0.0 : (x > 100.0) ? 100.0 : x)
DT_MODULE_INTROSPECTION(1, dt_iop_bloom_params_t)

typedef struct dt_iop_bloom_params_t
{
  float size;
  float threshold;
  float strength;
} dt_iop_bloom_params_t;

typedef struct dt_iop_bloom_gui_data_t
{
  GtkBox *vbox;
  GtkWidget *label1, *label2, *label3; // size,threshold,strength
  GtkWidget *scale1, *scale2, *scale3; // size,threshold,strength
} dt_iop_bloom_gui_data_t;

typedef struct dt_iop_bloom_data_t
{
  float size;
  float threshold;
  float strength;
} dt_iop_bloom_data_t;

typedef struct dt_iop_bloom_global_data_t
{
  int kernel_bloom_threshold;
  int kernel_bloom_hblur;
  int kernel_bloom_vblur;
  int kernel_bloom_mix;
} dt_iop_bloom_global_data_t;

const char *name()
{
  return _("bloom");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int groups()
{
  return dt_iop_get_group("bloom", IOP_GROUP_EFFECT);
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "size"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "threshold"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "strength"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  const dt_iop_bloom_gui_data_t *g = (dt_iop_bloom_gui_data_t *)self->gui_data;
  dt_accel_connect_slider_iop(self, "size", GTK_WIDGET(g->scale1));
  dt_accel_connect_slider_iop(self, "threshold", GTK_WIDGET(g->scale2));
  dt_accel_connect_slider_iop(self, "strength", GTK_WIDGET(g->scale3));
}

#define GAUSS(a, b, c, x) (a * pow(2.718281828, (-pow((x - b), 2) / (pow(c, 2)))))


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_bloom_data_t *data = (dt_iop_bloom_data_t *)piece->data;
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

  /* gather light by threshold */
  float *blurlightness = calloc((size_t)roi_out->width * roi_out->height, sizeof(float));
  memcpy(out, in, (size_t)roi_out->width * roi_out->height * ch * sizeof(float));

  const int rad = 256.0f * (fmin(100.0f, data->size + 1.0f) / 100.0f);
  const float _r = ceilf(rad * roi_in->scale / piece->iscale);
  const int radius = MIN(256.0f, _r);

  const float scale = 1.0f / exp2f(-1.0f * (fmin(100.0f, data->strength + 1.0f) / 100.0f));

/* get the thresholded lights into buffer */
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(data, blurlightness) schedule(static)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    float *inp = ((float *)ivoid) + ch * k;
    const float L = inp[0] * scale;
    if(L > data->threshold) blurlightness[k] = L;
  }


  /* horizontal blur into memchannel lightness */
  const int range = 2 * radius + 1;
  const int hr = range / 2;

  const size_t size = roi_out->width > roi_out->height ? roi_out->width : roi_out->height;
  float *const scanline_buf = malloc(size * dt_get_num_threads() * sizeof(float));

  for(int iteration = 0; iteration < BOX_ITERATIONS; iteration++)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(blurlightness) schedule(static)
#endif
    for(int y = 0; y < roi_out->height; y++)
    {
      float *scanline = scanline_buf + size * dt_get_thread_num();
      float L = 0;
      int hits = 0;
      const size_t index = (size_t)y * roi_out->width;
      for(int x = -hr; x < roi_out->width; x++)
      {
        int op = x - hr - 1;
        int np = x + hr;
        if(op >= 0)
        {
          L -= blurlightness[index + op];
          hits--;
        }
        if(np < roi_out->width)
        {
          L += blurlightness[index + np];
          hits++;
        }
        if(x >= 0) scanline[x] = L / hits;
      }

      for(int x = 0; x < roi_out->width; x++) blurlightness[index + x] = scanline[x];
    }

    /* vertical pass on blurlightness */
    const int opoffs = -(hr + 1) * roi_out->width;
    const int npoffs = (hr)*roi_out->width;


#ifdef _OPENMP
#pragma omp parallel for default(none) shared(blurlightness) schedule(static)
#endif
    for(int x = 0; x < roi_out->width; x++)
    {
      float *scanline = scanline_buf + size * dt_get_thread_num();
      float L = 0;
      int hits = 0;
      size_t index = (size_t)x - hr * roi_out->width;
      for(int y = -hr; y < roi_out->height; y++)
      {
        int op = y - hr - 1;
        int np = y + hr;

        if(op >= 0)
        {
          L -= blurlightness[index + opoffs];
          hits--;
        }
        if(np < roi_out->height)
        {
          L += blurlightness[index + npoffs];
          hits++;
        }
        if(y >= 0) scanline[y] = L / hits;
        index += roi_out->width;
      }

      for(int y = 0; y < roi_out->height; y++) blurlightness[y * roi_out->width + x] = scanline[y];
    }
  }
  free(scanline_buf);

/* screen blend lightness with original */
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(in, out, data, blurlightness) schedule(static)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    float *inp = in + ch * k;
    float *outp = out + ch * k;
    outp[0] = 100.0f - (((100.0f - inp[0]) * (100.0f - blurlightness[k])) / 100.0f); // Screen blend
    outp[1] = inp[1];
    outp[2] = inp[2];
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);

  free(blurlightness);
}

#ifdef HAVE_OPENCL
static int bucket_next(unsigned int *state, unsigned int max)
{
  const unsigned int current = *state;
  const unsigned int next = (current >= max - 1 ? 0 : current + 1);

  *state = next;

  return next;
}

int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_bloom_data_t *d = (dt_iop_bloom_data_t *)piece->data;
  const dt_iop_bloom_global_data_t *gd = (dt_iop_bloom_global_data_t *)self->data;

  cl_int err = -999;
  cl_mem dev_tmp[NUM_BUCKETS] = { NULL };
  cl_mem dev_tmp1;
  cl_mem dev_tmp2;
  unsigned int state = 0;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const float threshold = d->threshold;

  const int rad = 256.0f * (fmin(100.0f, d->size + 1.0f) / 100.0f);
  const float _r = ceilf(rad * roi_in->scale / piece->iscale);
  const int radius = MIN(256.0f, _r);
  const float scale = 1.0f / exp2f(-1.0f * (fmin(100.0f, d->strength + 1.0f) / 100.0f));

  int hblocksize;
  dt_opencl_local_buffer_t hlocopt
    = (dt_opencl_local_buffer_t){ .xoffset = 2 * radius, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                  .cellsize = sizeof(float), .overhead = 0,
                                  .sizex = 1 << 16, .sizey = 1 };

  if(dt_opencl_local_buffer_opt(devid, gd->kernel_bloom_hblur, &hlocopt))
    hblocksize = hlocopt.sizex;
  else
    hblocksize = 1;

  int vblocksize;
  dt_opencl_local_buffer_t vlocopt
    = (dt_opencl_local_buffer_t){ .xoffset = 1, .xfactor = 1, .yoffset = 2 * radius, .yfactor = 1,
                                  .cellsize = sizeof(float), .overhead = 0,
                                  .sizex = 1, .sizey = 1 << 16 };

  if(dt_opencl_local_buffer_opt(devid, gd->kernel_bloom_vblur, &vlocopt))
    vblocksize = vlocopt.sizey;
  else
    vblocksize = 1;


  const size_t bwidth = ROUNDUP(width, hblocksize);
  const size_t bheight = ROUNDUP(height, vblocksize);

  size_t sizes[3];
  size_t local[3];

  for(int i = 0; i < NUM_BUCKETS; i++)
  {
    dev_tmp[i] = dt_opencl_alloc_device(devid, width, height, sizeof(float));
    if(dev_tmp[i] == NULL) goto error;
  }

  /* gather light by threshold */
  sizes[0] = ROUNDUPWD(width);
  sizes[1] = ROUNDUPHT(height);
  sizes[2] = 1;
  dev_tmp1 = dev_tmp[bucket_next(&state, NUM_BUCKETS)];
  dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_threshold, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_threshold, 1, sizeof(cl_mem), (void *)&dev_tmp1);
  dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_threshold, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_threshold, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_threshold, 4, sizeof(float), (void *)&scale);
  dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_threshold, 5, sizeof(float), (void *)&threshold);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_bloom_threshold, sizes);
  if(err != CL_SUCCESS) goto error;

  if(radius != 0)
    for(int i = 0; i < BOX_ITERATIONS; i++)
    {
      /* horizontal blur */
      sizes[0] = bwidth;
      sizes[1] = ROUNDUPHT(height);
      sizes[2] = 1;
      local[0] = hblocksize;
      local[1] = 1;
      local[2] = 1;
      dev_tmp2 = dev_tmp[bucket_next(&state, NUM_BUCKETS)];
      dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_hblur, 0, sizeof(cl_mem), (void *)&dev_tmp1);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_hblur, 1, sizeof(cl_mem), (void *)&dev_tmp2);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_hblur, 2, sizeof(int), (void *)&radius);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_hblur, 3, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_hblur, 4, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_hblur, 5, sizeof(int), (void *)&hblocksize);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_hblur, 6, (hblocksize + 2 * radius) * sizeof(float),
                               NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_bloom_hblur, sizes, local);
      if(err != CL_SUCCESS) goto error;


      /* vertical blur */
      sizes[0] = ROUNDUPWD(width);
      sizes[1] = bheight;
      sizes[2] = 1;
      local[0] = 1;
      local[1] = vblocksize;
      local[2] = 1;
      dev_tmp1 = dev_tmp[bucket_next(&state, NUM_BUCKETS)];
      dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_vblur, 0, sizeof(cl_mem), (void *)&dev_tmp2);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_vblur, 1, sizeof(cl_mem), (void *)&dev_tmp1);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_vblur, 2, sizeof(int), (void *)&radius);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_vblur, 3, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_vblur, 4, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_vblur, 5, sizeof(int), (void *)&vblocksize);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_vblur, 6, (vblocksize + 2 * radius) * sizeof(float),
                               NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_bloom_vblur, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }

  /* mixing out and in -> out */
  sizes[0] = ROUNDUPWD(width);
  sizes[1] = ROUNDUPHT(height);
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_mix, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_mix, 1, sizeof(cl_mem), (void *)&dev_tmp1);
  dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_mix, 2, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_mix, 3, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_bloom_mix, 4, sizeof(int), (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_bloom_mix, sizes);
  if(err != CL_SUCCESS) goto error;

  for(int i = 0; i < NUM_BUCKETS; i++)
    dt_opencl_release_mem_object(dev_tmp[i]);
  return TRUE;

error:
  for(int i = 0; i < NUM_BUCKETS; i++)
    dt_opencl_release_mem_object(dev_tmp[i]);
  dt_print(DT_DEBUG_OPENCL, "[opencl_bloom] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  const dt_iop_bloom_data_t *d = (dt_iop_bloom_data_t *)piece->data;

  const int rad = 256.0f * (fmin(100.0f, d->size + 1.0f) / 100.0f);
  const float _r = ceilf(rad * roi_in->scale / piece->iscale);
  const int radius = MIN(256.0f, _r);

  tiling->factor = 2.0f + NUM_BUCKETS * 0.25f; // in + out + NUM_BUCKETS * 0.25 tmp
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 5 * radius; // This is a guess. TODO: check if that's sufficiently large
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 12; // bloom.cl, from programs.conf
  dt_iop_bloom_global_data_t *gd = (dt_iop_bloom_global_data_t *)malloc(sizeof(dt_iop_bloom_global_data_t));
  module->data = gd;
  gd->kernel_bloom_threshold = dt_opencl_create_kernel(program, "bloom_threshold");
  gd->kernel_bloom_hblur = dt_opencl_create_kernel(program, "bloom_hblur");
  gd->kernel_bloom_vblur = dt_opencl_create_kernel(program, "bloom_vblur");
  gd->kernel_bloom_mix = dt_opencl_create_kernel(program, "bloom_mix");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  const dt_iop_bloom_global_data_t *gd = (dt_iop_bloom_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_bloom_threshold);
  dt_opencl_free_kernel(gd->kernel_bloom_hblur);
  dt_opencl_free_kernel(gd->kernel_bloom_vblur);
  dt_opencl_free_kernel(gd->kernel_bloom_mix);
  free(module->data);
  module->data = NULL;
}

static void strength_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_bloom_params_t *p = (dt_iop_bloom_params_t *)self->params;
  p->strength = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void threshold_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_bloom_params_t *p = (dt_iop_bloom_params_t *)self->params;
  p->threshold = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void size_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_bloom_params_t *p = (dt_iop_bloom_params_t *)self->params;
  p->size = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_bloom_params_t *p = (dt_iop_bloom_params_t *)p1;
  dt_iop_bloom_data_t *d = (dt_iop_bloom_data_t *)piece->data;

  d->strength = p->strength;
  d->size = p->size;
  d->threshold = p->threshold;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_bloom_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  const dt_iop_module_t *module = (dt_iop_module_t *)self;
  const dt_iop_bloom_params_t *p = (dt_iop_bloom_params_t *)module->params;
  dt_iop_bloom_gui_data_t *g = (dt_iop_bloom_gui_data_t *)self->gui_data;
  dt_bauhaus_slider_set(g->scale1, p->size);
  dt_bauhaus_slider_set(g->scale2, p->threshold);
  dt_bauhaus_slider_set(g->scale3, p->strength);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_bloom_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_bloom_params_t));
  module->default_enabled = 0;
  module->priority = 514; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_bloom_params_t);
  module->gui_data = NULL;
  dt_iop_bloom_params_t tmp = (dt_iop_bloom_params_t){ 20, 90, 25 };
  memcpy(module->params, &tmp, sizeof(dt_iop_bloom_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_bloom_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_bloom_gui_data_t));
  dt_iop_bloom_gui_data_t *g = (dt_iop_bloom_gui_data_t *)self->gui_data;
  const dt_iop_bloom_params_t *p = (dt_iop_bloom_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  /* size */
  g->scale1 = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1.0, p->size, 0);
  dt_bauhaus_slider_set_format(g->scale1, "%.0f%%");
  dt_bauhaus_widget_set_label(g->scale1, NULL, _("size"));
  gtk_widget_set_tooltip_text(g->scale1, _("the size of bloom"));

  /* threshold */
  g->scale2 = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1.0, p->threshold, 0);
  dt_bauhaus_slider_set_format(g->scale2, "%.0f%%");
  dt_bauhaus_widget_set_label(g->scale2, NULL, _("threshold"));
  gtk_widget_set_tooltip_text(g->scale2, _("the threshold of light"));

  /* strength */
  g->scale3 = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1.0, p->strength, 0);
  dt_bauhaus_slider_set_format(g->scale3, "%.0f%%");
  dt_bauhaus_widget_set_label(g->scale3, NULL, _("strength"));
  gtk_widget_set_tooltip_text(g->scale3, _("the strength of bloom"));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(g->scale1), "value-changed", G_CALLBACK(size_callback), self);
  g_signal_connect(G_OBJECT(g->scale2), "value-changed", G_CALLBACK(threshold_callback), self);
  g_signal_connect(G_OBJECT(g->scale3), "value-changed", G_CALLBACK(strength_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
