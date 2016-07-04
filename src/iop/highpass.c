/*
    This file is part of darktable,
    copyright (c) 2011-2012 Henrik Andersson, ulrich pegelow.

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
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define MAX_RADIUS 16
#define BOX_ITERATIONS 8
#define BLOCKSIZE                                                                                            \
  2048 /* maximum blocksize. must be a power of 2 and will be automatically reduced if needed */

#define CLIP(x) ((x < 0) ? 0.0 : (x > 1.0) ? 1.0 : x)
#define LCLIP(x) ((x < 0) ? 0.0 : (x > 100.0) ? 100.0 : x)

DT_MODULE_INTROSPECTION(1, dt_iop_highpass_params_t)

typedef struct dt_iop_highpass_params_t
{
  float sharpness;
  float contrast;
} dt_iop_highpass_params_t;

typedef struct dt_iop_highpass_gui_data_t
{
  GtkBox *vbox1, *vbox2;
  GtkWidget *label1, *label2; // sharpness,contrast
  GtkWidget *scale1, *scale2; // sharpness,contrast
} dt_iop_highpass_gui_data_t;

typedef struct dt_iop_highpass_data_t
{
  float sharpness;
  float contrast;
} dt_iop_highpass_data_t;

typedef struct dt_iop_highpass_global_data_t
{
  int kernel_highpass_invert;
  int kernel_highpass_hblur;
  int kernel_highpass_vblur;
  int kernel_highpass_mix;
} dt_iop_highpass_global_data_t;


const char *name()
{
  return _("highpass");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int groups()
{
  return IOP_GROUP_EFFECT;
}

#if 0 // BAUHAUS doesn't support keyaccels yet...
void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "sharpness"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "contrast boost"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_highpass_gui_data_t *g =
    (dt_iop_highpass_gui_data_t*)self->gui_data;

  dt_accel_connect_slider_iop(self, "sharpness", GTK_WIDGET(g->scale1));
  dt_accel_connect_slider_iop(self, "contrast boost", GTK_WIDGET(g->scale2));
}
#endif

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_highpass_data_t *d = (dt_iop_highpass_data_t *)piece->data;

  int rad = MAX_RADIUS * (fmin(100.0f, d->sharpness + 1) / 100.0f);
  const int radius = MIN(MAX_RADIUS, ceilf(rad * roi_in->scale / piece->iscale));

  const float sigma = sqrt((radius * (radius + 1) * BOX_ITERATIONS + 2) / 3.0f);
  const int wdh = ceilf(3.0f * sigma);

  tiling->factor = 3.0f; // in + out + tmp
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = wdh;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_highpass_data_t *d = (dt_iop_highpass_data_t *)piece->data;
  dt_iop_highpass_global_data_t *gd = (dt_iop_highpass_global_data_t *)self->data;

  cl_int err = -999;
  cl_mem dev_tmp = NULL;
  cl_mem dev_m = NULL;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;


  int rad = MAX_RADIUS * (fmin(100.0f, d->sharpness + 1) / 100.0f);
  const int radius = MIN(MAX_RADIUS, ceilf(rad * roi_in->scale / piece->iscale));

  /* sigma-radius correlation to match opencl vs. non-opencl. identified by numerical experiments but
   * unproven. ask me if you need details. ulrich */
  const float sigma = sqrt((radius * (radius + 1) * BOX_ITERATIONS + 2) / 3.0f);
  const int wdh = ceilf(3.0f * sigma);
  const int wd = 2 * wdh + 1;
  float mat[wd];
  float *m = mat + wdh;
  float weight = 0.0f;

  // init gaussian kernel
  for(int l = -wdh; l <= wdh; l++) weight += m[l] = expf(-(l * l) / (2.f * sigma * sigma));
  for(int l = -wdh; l <= wdh; l++) m[l] /= weight;

  // for(int l=-wdh; l<=wdh; l++) printf("%.6f ", (double)m[l]);
  // printf("\n");

  float contrast_scale = ((d->contrast / 100.0f) * 7.5f);

  size_t maxsizes[3] = { 0 };     // the maximum dimensions for a work group
  size_t workgroupsize = 0;       // the maximum number of items in a work group
  unsigned long localmemsize = 0; // the maximum amount of local memory we can use
  size_t kernelworkgroupsize = 0; // the maximum amount of items in work group for this kernel

  // make sure blocksize is not too large
  int blocksize = BLOCKSIZE;
  if(dt_opencl_get_work_group_limits(devid, maxsizes, &workgroupsize, &localmemsize) == CL_SUCCESS
     && dt_opencl_get_kernel_work_group_size(devid, gd->kernel_highpass_hblur, &kernelworkgroupsize)
        == CL_SUCCESS)
  {
    // reduce blocksize step by step until it fits to limits
    while(blocksize > maxsizes[0] || blocksize > maxsizes[1] || blocksize > kernelworkgroupsize
          || blocksize > workgroupsize || (blocksize + 2 * wdh) * sizeof(float) > localmemsize)
    {
      if(blocksize == 1) break;
      blocksize >>= 1;
    }
  }
  else
  {
    blocksize = 1; // slow but safe
  }

  const size_t bwidth = width % blocksize == 0 ? width : (width / blocksize + 1) * blocksize;
  const size_t bheight = height % blocksize == 0 ? height : (height / blocksize + 1) * blocksize;

  size_t sizes[3];
  size_t local[3];

  dev_tmp = dt_opencl_alloc_device(devid, width, height, 4 * sizeof(float));
  if(dev_tmp == NULL) goto error;

  dev_m = dt_opencl_copy_host_to_device_constant(devid, (size_t)sizeof(float) * wd, mat);
  if(dev_m == NULL) goto error;

  /* invert image */
  sizes[0] = ROUNDUPWD(width);
  sizes[1] = ROUNDUPHT(height);
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_invert, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_invert, 1, sizeof(cl_mem), (void *)&dev_tmp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_invert, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_invert, 3, sizeof(int), (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highpass_invert, sizes);
  if(err != CL_SUCCESS) goto error;

  if(rad != 0)
  {
    /* horizontal blur */
    sizes[0] = bwidth;
    sizes[1] = ROUNDUPHT(height);
    sizes[2] = 1;
    local[0] = blocksize;
    local[1] = 1;
    local[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 0, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 2, sizeof(cl_mem), (void *)&dev_m);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 3, sizeof(int), (void *)&wdh);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 4, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 5, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 6, sizeof(int), (void *)&blocksize);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 7, (blocksize + 2 * wdh) * sizeof(float), NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highpass_hblur, sizes, local);
    if(err != CL_SUCCESS) goto error;


    /* vertical blur */
    sizes[0] = ROUNDUPWD(width);
    sizes[1] = bheight;
    sizes[2] = 1;
    local[0] = 1;
    local[1] = blocksize;
    local[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 0, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 1, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 2, sizeof(cl_mem), (void *)&dev_m);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 3, sizeof(int), (void *)&wdh);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 4, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 5, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 6, sizeof(int), (void *)&blocksize);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 7, (blocksize + 2 * wdh) * sizeof(float), NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highpass_vblur, sizes, local);
    if(err != CL_SUCCESS) goto error;
  }

  /* mixing tmp and in -> out */
  sizes[0] = ROUNDUPWD(width);
  sizes[1] = ROUNDUPHT(height);
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_mix, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_mix, 1, sizeof(cl_mem), (void *)&dev_tmp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_mix, 2, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_mix, 3, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_mix, 4, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_mix, 5, sizeof(float), (void *)&contrast_scale);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highpass_mix, sizes);
  if(err != CL_SUCCESS) goto error;

  if(dev_m != NULL) dt_opencl_release_mem_object(dev_m);
  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  return TRUE;

error:
  if(dev_m != NULL) dt_opencl_release_mem_object(dev_m);
  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  dt_print(DT_DEBUG_OPENCL, "[opencl_highpass] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_highpass_data_t *data = (dt_iop_highpass_data_t *)piece->data;
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

/* create inverted image and then blur */
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(in, out) schedule(static)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
    out[ch * k] = 100.0f - LCLIP(in[ch * k]); // only L in Lab space


  int rad = MAX_RADIUS * (fmin(100.0, data->sharpness + 1) / 100.0);
  const int radius = MIN(MAX_RADIUS, ceilf(rad * roi_in->scale / piece->iscale));

  /* horizontal blur out into out */
  const int range = 2 * radius + 1;
  const int hr = range / 2;

  const int size = roi_out->width > roi_out->height ? roi_out->width : roi_out->height;
  float *scanline = calloc(size, sizeof(float));

  for(int iteration = 0; iteration < BOX_ITERATIONS; iteration++)
  {
    for(int y = 0; y < roi_out->height; y++)
    {
      float L = 0;
      int hits = 0;
      size_t index = (size_t)y * roi_out->width;
      for(int x = -hr; x < roi_out->width; x++)
      {
        int op = x - hr - 1;
        int np = x + hr;
        if(op >= 0)
        {
          L -= out[(index + op) * ch];
          hits--;
        }
        if(np < roi_out->width)
        {
          L += out[(index + np) * ch];
          hits++;
        }
        if(x >= 0) scanline[x] = L / hits;
      }

      for(int x = 0; x < roi_out->width; x++) out[(index + x) * ch] = scanline[x];
    }

    /* vertical pass on blurlightness */
    const int opoffs = -(hr + 1) * roi_out->width;
    const int npoffs = (hr)*roi_out->width;
    for(int x = 0; x < roi_out->width; x++)
    {
      float L = 0;
      int hits = 0;
      size_t index = (size_t)x - hr * roi_out->width;
      for(int y = -hr; y < roi_out->height; y++)
      {
        int op = y - hr - 1;
        int np = y + hr;
        if(op >= 0)
        {
          L -= out[(index + opoffs) * ch];
          hits--;
        }
        if(np < roi_out->height)
        {
          L += out[(index + npoffs) * ch];
          hits++;
        }
        if(y >= 0) scanline[y] = L / hits;
        index += roi_out->width;
      }

      for(int y = 0; y < roi_out->height; y++) out[((size_t)y * roi_out->width + x) * ch] = scanline[y];
    }
  }

  free(scanline);

  const float contrast_scale = ((data->contrast / 100.0) * 7.5);
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(in, out, data) schedule(static)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    size_t index = ch * k;
    // Mix out and in
    out[index] = out[index] * 0.5 + in[index] * 0.5;
    out[index] = LCLIP(50.0f + ((out[index] - 50.0f) * contrast_scale));
    out[index + 1] = out[index + 2] = 0.0f; // desaturate a and b in Lab space
    out[index + 3] = in[index + 3];
  }
}

static void sharpness_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)self->params;
  p->sharpness = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void contrast_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)self->params;
  p->contrast = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)p1;
  dt_iop_highpass_data_t *d = (dt_iop_highpass_data_t *)piece->data;

  d->sharpness = p->sharpness;
  d->contrast = p->contrast;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_highpass_data_t));
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
  dt_iop_highpass_gui_data_t *g = (dt_iop_highpass_gui_data_t *)self->gui_data;
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)module->params;
  dt_bauhaus_slider_set(g->scale1, p->sharpness);
  dt_bauhaus_slider_set(g->scale2, p->contrast);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_highpass_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_highpass_params_t));
  module->default_enabled = 0;
  module->priority = 769; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_highpass_params_t);
  module->gui_data = NULL;
  dt_iop_highpass_params_t tmp = (dt_iop_highpass_params_t){ 50, 50 };
  memcpy(module->params, &tmp, sizeof(dt_iop_highpass_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_highpass_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 4; // highpass.cl, from programs.conf
  dt_iop_highpass_global_data_t *gd
      = (dt_iop_highpass_global_data_t *)malloc(sizeof(dt_iop_highpass_global_data_t));
  module->data = gd;
  gd->kernel_highpass_invert = dt_opencl_create_kernel(program, "highpass_invert");
  gd->kernel_highpass_hblur = dt_opencl_create_kernel(program, "highpass_hblur");
  gd->kernel_highpass_vblur = dt_opencl_create_kernel(program, "highpass_vblur");
  gd->kernel_highpass_mix = dt_opencl_create_kernel(program, "highpass_mix");
}


void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_highpass_global_data_t *gd = (dt_iop_highpass_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_highpass_invert);
  dt_opencl_free_kernel(gd->kernel_highpass_hblur);
  dt_opencl_free_kernel(gd->kernel_highpass_vblur);
  dt_opencl_free_kernel(gd->kernel_highpass_mix);
  free(module->data);
  module->data = NULL;
}


void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_highpass_gui_data_t));
  dt_iop_highpass_gui_data_t *g = (dt_iop_highpass_gui_data_t *)self->gui_data;
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  /* sharpness */
  g->scale1 = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 0.5, p->sharpness, 2);
  dt_bauhaus_widget_set_label(g->scale1, NULL, _("sharpness"));
  dt_bauhaus_slider_set_format(g->scale1, "%.0f%%");
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->scale1, _("the sharpness of highpass filter"));
  g_signal_connect(G_OBJECT(g->scale1), "value-changed", G_CALLBACK(sharpness_callback), self);

  /* contrast boost */
  g->scale2 = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 0.5, p->contrast, 2);
  dt_bauhaus_widget_set_label(g->scale2, NULL, _("contrast boost"));
  dt_bauhaus_slider_set_format(g->scale2, "%.0f%%");
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->scale2, _("the contrast of highpass filter"));
  g_signal_connect(G_OBJECT(g->scale2), "value-changed", G_CALLBACK(contrast_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
