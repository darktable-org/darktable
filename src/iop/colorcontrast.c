/* -*- Mode: c; c-basic-offset: 2; -*- */
/*
    This file is part of darktable,
    Copyright (C) 2011-2023 darktable developers.


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
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE_INTROSPECTION(2, dt_iop_colorcontrast_params_t)

typedef struct dt_iop_colorcontrast_params_t
{
  float a_steepness; // $MIN: 0.0 $MAX: 5.0 $DEFAULT: 1.0 $DESCRIPTION: "green-magenta contrast"
  float a_offset;
  float b_steepness; // $MIN: 0.0 $MAX: 5.0 $DEFAULT: 1.0 $DESCRIPTION: "blue-yellow contrast"
  float b_offset;
  int unbound;       // $DEFAULT: 1
} dt_iop_colorcontrast_params_t;

typedef struct dt_iop_colorcontrast_gui_data_t
{
  // whatever you need to make your gui happy.
  // stored in self->gui_data
  GtkBox *vbox;
  GtkWidget *a_scale; // this is needed by gui_update
  GtkWidget *b_scale;
} dt_iop_colorcontrast_gui_data_t;

typedef struct dt_iop_colorcontrast_data_t
{
  // this is stored in the pixelpipeline after a commit (not the db),
  // you can do some precomputation and get this data in process().
  // stored in piece->data
  float a_steepness;
  float a_offset;
  float b_steepness;
  float b_offset;
  int unbound;
} dt_iop_colorcontrast_data_t;

typedef struct dt_iop_colorcontrast_global_data_t
{
  int kernel_colorcontrast;
} dt_iop_colorcontrast_global_data_t;


const char *name()
{
  return _("color contrast");
}

const char *aliases()
{
  return _("saturation");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("increase saturation and separation between\n"
                                        "opposite colors"),
                                      _("creative"),
                                      _("non-linear, Lab, display-referred"),
                                      _("non-linear, Lab"),
                                      _("non-linear, Lab, display-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_GRADING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_colorcontrast_params_v2_t
  {
    float a_steepness;
    float a_offset;
    float b_steepness;
    float b_offset;
    int unbound;
  } dt_iop_colorcontrast_params_v2_t;

  if(old_version == 1)
  {
    typedef struct dt_iop_colorcontrast_params_v1_t
    {
      float a_steepness;
      float a_offset;
      float b_steepness;
      float b_offset;
    } dt_iop_colorcontrast_params_v1_t;

    const dt_iop_colorcontrast_params_v1_t *o = old_params;
    dt_iop_colorcontrast_params_v2_t *n = malloc(sizeof(dt_iop_colorcontrast_params_v2_t));

    n->a_steepness = o->a_steepness;
    n->a_offset = o->a_offset;
    n->b_steepness = o->b_steepness;
    n->b_offset = o->b_offset;
    n->unbound = 0;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_colorcontrast_params_v2_t);
    *new_version = 2;
    return 0;
  }
  return 1;
}

DT_OMP_DECLARE_SIMD(aligned(in,out:64) aligned(slope,offset,low,high))
static inline void clamped_scaling(float *const restrict out,
                                   const float *const restrict in,
                                   const dt_aligned_pixel_t slope,
                                   const dt_aligned_pixel_t offset,
                                   const dt_aligned_pixel_t low,
                                   const dt_aligned_pixel_t high)
{
  dt_aligned_pixel_t res;
  for_each_channel(c)
    res[c] = CLAMPS(in[c] * slope[c] + offset[c], low[c], high[c]);
  copy_pixel_nontemporal(out, res);
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.

  // get our data struct:
  const dt_iop_colorcontrast_params_t *const d = piece->data;

  // how many colors in our buffer?
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self,
                                        piece->colors,
                                        ivoid, ovoid, roi_in, roi_out))
    return; // image has been copied through to output and module's
            // trouble flag has been updated

  const float *const restrict in = DT_IS_ALIGNED((const float *const)ivoid);
  float *const restrict out = DT_IS_ALIGNED((float *const)ovoid);
  const size_t npixels = (size_t)roi_out->width * roi_out->height;

  const dt_aligned_pixel_t slope = { 1.0f, d->a_steepness, d->b_steepness, 1.0f };
  const dt_aligned_pixel_t offset = { 0.0f, d->a_offset, d->b_offset, 0.0f };
  const dt_aligned_pixel_t lowlimit = { -FLT_MAX, -128.0f, -128.0f, -FLT_MAX };
  const dt_aligned_pixel_t highlimit = { FLT_MAX, 128.0f, 128.0f, FLT_MAX };

  if(d->unbound)
  {
    DT_OMP_FOR()
    for(size_t k = 0; k < (size_t)4 * npixels; k += 4)
    {
      dt_aligned_pixel_t res;
      for_each_channel(c)
      {
        res[c] = (in[k + c] * slope[c]) + offset[c];
      }
      copy_pixel_nontemporal(out + k, res);
    }
  }
  else
  {

    DT_OMP_FOR()
    for(size_t k = 0; k < npixels; k ++)
    {
      // the inner per-pixel loop needs to be declared in a separate
      // vectorizable function to convince the compiler that it
      // doesn't need to check for overlap or misalignment of the
      // buffers for *every* pixel, which actually makes the code
      // slower than not vectorizing....
      clamped_scaling(out + 4*k, in + 4*k, slope, offset, lowlimit, highlimit);
    }
  }
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorcontrast_data_t *data = piece->data;
  dt_iop_colorcontrast_global_data_t *gd = self->global_data;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const float scale[4] = { 1.0f, data->a_steepness, data->b_steepness, 1.0f };
  const float offset[4] = { 0.0f, data->a_offset, data->b_offset, 0.0f };
  const int unbound = data->unbound;

  return dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_colorcontrast, width, height,
                                     CLARG(dev_in), CLARG(dev_out),
                                     CLARG(width), CLARG(height),
                                     CLARG(scale), CLARG(offset), CLARG(unbound));
}
#endif


void init_global(dt_iop_module_so_t *self)
{
  const int program = 8; // extended.cl, from programs.conf
  dt_iop_colorcontrast_global_data_t *gd = malloc(sizeof(dt_iop_colorcontrast_global_data_t));
  self->data = gd;
  gd->kernel_colorcontrast = dt_opencl_create_kernel(program, "colorcontrast");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_colorcontrast_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_colorcontrast);
  free(self->data);
  self->data = NULL;
}


/** commit is the synch point between core and gui, so it copies params to pipe data. */
void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *params,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorcontrast_params_t *p = (dt_iop_colorcontrast_params_t *)params;
  dt_iop_colorcontrast_data_t *d = piece->data;
  d->a_steepness = p->a_steepness;
  d->a_offset = p->a_offset;
  d->b_steepness = p->b_steepness;
  d->b_offset = p->b_offset;
  d->unbound = p->unbound;
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_colorcontrast_data_t));
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_colorcontrast_gui_data_t *g = self->gui_data;
  dt_iop_colorcontrast_params_t *p = self->params;
  dt_bauhaus_slider_set(g->a_scale, p->a_steepness);
  dt_bauhaus_slider_set(g->b_scale, p->b_steepness);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_colorcontrast_gui_data_t *g = IOP_GUI_ALLOC(colorcontrast);

  g->a_scale = dt_bauhaus_slider_from_params(self, "a_steepness");
  gtk_widget_set_tooltip_text
    (g->a_scale,
     _("steepness of the a* curve in Lab\nlower values desaturate"
       " greens and magenta while higher saturate them"));

  g->b_scale = dt_bauhaus_slider_from_params(self, "b_steepness");
  gtk_widget_set_tooltip_text
    (g->b_scale,

     _("steepness of the b* curve in Lab\nlower values desaturate"
       " blues and yellows while higher saturate them"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
