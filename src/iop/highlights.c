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
#include <xmmintrin.h>
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "bauhaus/bauhaus.h"
#include "common/opencl.h"
#include <gtk/gtk.h>
#include <inttypes.h>


DT_MODULE_INTROSPECTION(2, dt_iop_highlights_params_t)

typedef enum dt_iop_highlights_mode_t
{
  DT_IOP_HIGHLIGHTS_CLIP = 0,
  DT_IOP_HIGHLIGHTS_LCH = 1
}
dt_iop_highlights_mode_t;

typedef struct dt_iop_highlights_params_t
{
  dt_iop_highlights_mode_t mode;
  float blendL, blendC, blendh; // unused
  float clip;
}
dt_iop_highlights_params_t;

typedef struct dt_iop_highlights_gui_data_t
{
  GtkWidget *clip;
  GtkWidget *mode;
}
dt_iop_highlights_gui_data_t;

typedef dt_iop_highlights_params_t dt_iop_highlights_data_t;

typedef struct dt_iop_highlights_global_data_t
{
  int kernel_highlights_1f;
  int kernel_highlights_4f;
}
dt_iop_highlights_global_data_t;

const char *name()
{
  return _("highlight reconstruction");
}

int
groups ()
{
  return IOP_GROUP_BASIC;
}

int
flags ()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

int
legacy_params (dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    memcpy(new_params, old_params, sizeof(dt_iop_highlights_params_t)-sizeof(float));
    dt_iop_highlights_params_t *n = (dt_iop_highlights_params_t *)new_params;
    n->clip = 1.0f;
    return 0;
  }
  return 1;
}

#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1};
  const float clip = d->clip * fminf(piece->pipe->processed_maximum[0], fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));
  const int filters = dt_image_flipped_filter(&piece->pipe->image);
  if(dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) || !filters)
  {
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f, 4, sizeof(int), (void *)&d->mode);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f, 5, sizeof(float), (void *)&clip);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_4f, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f, 4, sizeof(int), (void *)&d->mode);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f, 5, sizeof(float), (void *)&clip);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f, 6, sizeof(int), (void *)&roi_out->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f, 7, sizeof(int), (void *)&roi_out->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f, 8, sizeof(int), (void *)&filters);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_1f, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_highlights] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

int
output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  if(!dt_dev_pixelpipe_uses_downsampled_input(pipe) && (pipe->image.flags & DT_IMAGE_RAW)) return sizeof(float);
  else return 4*sizeof(float);
}


void process(
    struct dt_iop_module_t *self,
    dt_dev_pixelpipe_iop_t *piece,
    void *ivoid,
    void *ovoid,
    const dt_iop_roi_t *roi_in,
    const dt_iop_roi_t *roi_out)
{
  const int filters = dt_image_flipped_filter(&piece->pipe->image);
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;

  const float clip = data->clip * fminf(piece->pipe->processed_maximum[0], fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));
  // const int ch = piece->colors;
  if(dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) || !filters)
  {
    const __m128 clipm = _mm_set1_ps(clip);
#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic) default(none) shared(ovoid, ivoid, roi_in, roi_out, data, piece)
#endif
    for(int j=0; j<roi_out->height; j++)
    {
      float *out = (float *)ovoid + (size_t)4*roi_out->width*j;
      float *in  = (float *)ivoid + (size_t)4*roi_in->width*j;
      for(int i=0; i<roi_out->width; i++)
      {
        _mm_stream_ps(out, _mm_min_ps(clipm, _mm_set_ps(in[3],in[2],in[1],in[0])));
        in += 4;
        out += 4;
      }
    }
    _mm_sfence();
    return;
  }

  switch(data->mode)
  {
    case DT_IOP_HIGHLIGHTS_LCH:
#ifdef _OPENMP
      #pragma omp parallel for schedule(dynamic) default(none) shared(ovoid, ivoid, roi_in, roi_out, data, piece)
#endif
      for(int j=0; j<roi_out->height; j++)
      {
        float *out = (float *)ovoid + (size_t)roi_out->width*j;
        float *in  = (float *)ivoid + (size_t)roi_out->width*j;
        for(int i=0; i<roi_out->width; i++)
        {
          if(i==0 || i==roi_out->width-1 || j==0 || j==roi_out->height-1)
          {
            // fast path for border
            out[0] = in[0];
          }
          else
          {
            // analyse one bayer block to get same number of rggb pixels each time
            const float near_clip = 0.96f*clip;
            const float post_clip = 1.10f*clip;
            float blend = 0.0f;
            float mean = 0.0f;
            for(int jj=0; jj<=1; jj++)
            {
              for(int ii=0; ii<=1; ii++)
              {
                const float val = in[(size_t)jj*roi_out->width + ii];
                mean += val*0.25f;
                blend += (fminf(post_clip, val) - near_clip)/(post_clip-near_clip);
              }
            }
            blend = CLAMP(blend, 0.0f, 1.0f);
            if(blend > 0)
            {
              // recover:
              out[0] = blend*mean + (1.f-blend)*in[0];
            }
            else out[0] = in[0];
          }
          out ++;
          in ++;
        }
      }
      break;
    default:
    case DT_IOP_HIGHLIGHTS_CLIP:
    {
      const __m128 clipm = _mm_set1_ps(clip);
      const size_t n = (size_t)roi_out->height*roi_out->width;
      float *const out = (float *)ovoid;
      float *const in  = (float *)ivoid;
#ifdef _OPENMP
      #pragma omp parallel for schedule(static) default(none)
#endif
      for(int j=0; j<n; j+=4)
        _mm_stream_ps(out+j, _mm_min_ps(clipm, _mm_load_ps(in+j)));
      _mm_sfence();
      // lets see if there's a non-multiple of four rest to process:
      if(n & 3) for(size_t j=n&~3u; j<n; j++) out[j] = MIN(clip, in[j]);
      break;
    }
  }

  if(piece->pipe->mask_display)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

static void
clip_callback (GtkWidget *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;
  p->clip = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
mode_changed (GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;
  int active = dt_bauhaus_combobox_get(combo);

  switch(active)
  {
    case DT_IOP_HIGHLIGHTS_CLIP:
      p->mode = DT_IOP_HIGHLIGHTS_CLIP;
      break;
    default:
    case DT_IOP_HIGHLIGHTS_LCH:
      p->mode = DT_IOP_HIGHLIGHTS_LCH;
      break;
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)p1;
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  memcpy(d, p, sizeof(*p));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)malloc(sizeof(dt_iop_highlights_global_data_t));
  module->data = gd;
  gd->kernel_highlights_1f = dt_opencl_create_kernel(program, "highlights_1f");
  gd->kernel_highlights_4f = dt_opencl_create_kernel(program, "highlights_4f");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_highlights_4f);
  dt_opencl_free_kernel(gd->kernel_highlights_1f);
  free(module->data);
  module->data = NULL;
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_highlights_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)module->params;
  dt_bauhaus_slider_set(g->clip,   p->clip);
  dt_bauhaus_combobox_set(g->mode, p->mode);
}

void reload_defaults(dt_iop_module_t *module)
{
  // only on for raw images:
  if(dt_image_is_raw(&module->dev->image_storage))
    module->default_enabled = 1;
  else
    module->default_enabled = 0;

  dt_iop_highlights_params_t tmp = (dt_iop_highlights_params_t)
  {
    0, 1.0, 0.0, 0.0, 1.0
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_highlights_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_highlights_params_t));
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_highlights_data_t));
  module->params = malloc(sizeof(dt_iop_highlights_params_t));
  module->default_params = malloc(sizeof(dt_iop_highlights_params_t));
  module->priority = 52; // module order created by iop_dependencies.py, do not edit!
  module->default_enabled = 1;
  module->params_size = sizeof(dt_iop_highlights_params_t);
  module->gui_data = NULL;
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
  self->gui_data = malloc(sizeof(dt_iop_highlights_gui_data_t));
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;

  self->widget = gtk_vbox_new(FALSE, DT_BAUHAUS_SPACE);

  g->mode = dt_bauhaus_combobox_new(self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->mode, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->mode, NULL, _("method"));
  dt_bauhaus_combobox_add(g->mode, _("clip highlights"));
  dt_bauhaus_combobox_add(g->mode, _("reconstruct in LCh"));
  g_object_set(G_OBJECT(g->mode), "tooltip-text", _("highlight reconstruction method"), (char *)NULL);

  g->clip = dt_bauhaus_slider_new_with_range(self, 0.0, 2.0, 0.01, p->clip, 3);
  g_object_set(G_OBJECT(g->clip), "tooltip-text", _("manually adjust the clipping threshold against"
               " magenta highlights (you shouldn't ever need to touch this)"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->clip, NULL, _("clipping threshold"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->clip, TRUE, TRUE, 0);


  g_signal_connect (G_OBJECT (g->clip), "value-changed",
                    G_CALLBACK (clip_callback), self);
  g_signal_connect (G_OBJECT (g->mode), "value-changed",
                    G_CALLBACK (mode_changed), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
