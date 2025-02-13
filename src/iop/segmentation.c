/*
    This file is part of darktable,
    Copyright (C) 2025 darktable developers.

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
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bauhaus/bauhaus.h"
#include "develop/tiling.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#ifdef _OPENMP
#include <omp.h>
#endif

DT_MODULE_INTROSPECTION(1, dt_iop_segmentation_params_t)

typedef enum dt_iop_segmentation_model_t
{
  DT_IOP_SEGMENTATION_MODEL_FELSENZWALB = 0,  // $DESCRIPTION: "felsenzwalb"
  DT_IOP_SEGMENTATION_MODEL_FASTS_SAM = 1,    // $DESCRIPTION: "fast SAM"
  DT_IOP_SEGMENTATION_MODEL_OBJECT_SAM = 2,   // $DESCRIPTION: "content aware SAM"
} dt_iop_segmentation_model_t;

typedef struct dt_iop_segmentation_params_t
{
  dt_iop_segmentation_model_t model;  // $DEFAULT: DT_IOP_SEGMENTATION_MODEL_FASTS_SAM $DESCRIPTION: "model"
  float depth;                        // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.1 $DESCRIPTION: "segment depth"
} dt_iop_segmentation_params_t;

typedef struct dt_iop_segmentation_data_t
{
  dt_iop_segmentation_model_t model;
  float depth;
} dt_iop_segmentation_data_t;

typedef struct dt_iop_segmentation_global_data_t
{
  int dummy;
} dt_iop_segmentation_global_data_t;


const char *name()
{
  return _("AI segmentation");
}

const char *aliases()
{
  return _("AI masks");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
      _("generate segment masks"),
      _("corrective"),
      _("linear, RGB, scene-referred"),
      _("linear, RGB"),
      _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ONE_INSTANCE;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

typedef struct dt_iop_segmentation_gui_data_t
{
  GtkWidget *model;
  GtkWidget *depth;
} dt_iop_segmentation_gui_data_t;

/*
void init_global(dt_iop_module_so_t *module)
{
  dt_iop_segmentation_global_data_t *gd = malloc(sizeof(dt_iop_segmentation_global_data_t));
  module->data = gd;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_segmentation_global_data_t *gd = (dt_iop_segmentation_global_data_t *)module->data;

  free(gd);
  module->data = NULL;
}
*/

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  return 1;
}

static void _dummy_segmentation(dt_dev_segmentation_t *seg)
{
  const int width = 256;
  const int height = 160;
  const int segments = 4;
  uint8_t *map = dt_calloc_align_type(uint8_t, (size_t)width * height * segments);

  seg->swidth = width;  // segment dimension as that is later required for the mask scale & distortion
  seg->sheight = height;
  seg->segments = segments;
  seg->map = map;

  // just do something that can be seen
  DT_OMP_FOR()
  for(size_t row = 0; row < height; row++)
  {
    for(size_t col = 0; col < width; col++)
    {
      const size_t start = (size_t)segments * (row*width + col);
      if     ((row < height / 2) && (col < width / 2)) map[start] = 255;
      else if((row > height / 2) && (col < width / 2)) map[start+1] = 255;
      else if((row < height / 2) && (col > width / 2)) map[start+2] = 255;
      else map[start+3] = 255;
    }
  }
}

static char *_algo_name(const int model)
{
  switch(model)
  {
    case DT_IOP_SEGMENTATION_MODEL_FELSENZWALB: return "felsenzwalb";
    case DT_IOP_SEGMENTATION_MODEL_FASTS_SAM:   return "fast SAM";
    case DT_IOP_SEGMENTATION_MODEL_OBJECT_SAM:  return "content aware SAM";
    default:                                    return "unknown segmentation algorithm";
  }
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  float *input = (float *)ivoid;
  float *output = (float *)ovoid;

  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;
  dt_iop_image_copy(output, input, (size_t)iwidth * iheight * 4);

  dt_dev_pixelpipe_t *pipe = piece->pipe;
  dt_iop_segmentation_data_t *d = piece->data;
  dt_dev_segmentation_t *seg = &pipe->segmentation;

  const gboolean fullpipe = pipe->type & DT_DEV_PIXELPIPE_FULL;

  if(!pipe->want_segmentation)
  {
    dt_print_pipe(DT_DEBUG_PIPE,
      "segmentation none", pipe, self, pipe->devid, roi_in, roi_out);
    return;
  }

  const int hdata[6] = {(int)self->dev->image_storage.id,
                        (int)seg->model,
                        (int)d->model,
                        (int)iwidth,
                        (int)iheight,
                        (int)d->depth * 1e6f };
  const dt_hash_t hash = dt_hash(DT_INITHASH, &hdata, sizeof(hdata));

  if(seg->segments && (hash == seg->hash))
  {
    dt_print_pipe(DT_DEBUG_PIPE,
      "segmentation available", pipe, self, pipe->devid, roi_in, roi_out,
      "'%s`: %d segments %dx%d",
      _algo_name(seg->model), seg->segments, seg->swidth, seg->sheight);
    return;
  }

  const gboolean fullscale = darktable.develop->late_scaling.enabled
                          || darktable.develop->late_scaling.segmentation;

  // NOTE: requires further investigation for export and image pipes
  if(!fullscale && fullpipe)
  {
    dt_print_pipe(DT_DEBUG_PIPE,
      "segmentation up", pipe, self, pipe->devid, roi_in, roi_out);
    darktable.develop->late_scaling.segmentation = TRUE;
    dt_dev_reprocess_center(self->dev);
    return;
  }
  dt_dev_clear_segmentation(pipe);

  // define those that are shared for all algos
  seg->iwidth = iwidth;
  seg->iheight = iheight;
  seg->hash = hash;
  seg->model = d->model;

  // We can now process the AI segmentation algorithm, that must define the structs data
  switch(seg->model)
  {
    default: _dummy_segmentation(seg);
  }

  dt_print_pipe(DT_DEBUG_PIPE,
      "segmentation done", pipe, self, pipe->devid, roi_in, roi_out,
      "'%s`: %d segments %dx%d",
      _algo_name(seg->model), seg->segments, seg->swidth, seg->sheight);

  if(fullscale && fullpipe)
  {
    dt_print_pipe(DT_DEBUG_PIPE,
      "segmentation down", pipe, self, pipe->devid, roi_in, roi_out);
    darktable.develop->late_scaling.segmentation = FALSE;
    dt_dev_reprocess_center(self->dev);
  }
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_segmentation_params_t *p = (dt_iop_segmentation_params_t *)p1;
  dt_iop_segmentation_data_t *d = piece->data;

  d->depth = p->depth;
  d->model = p->model;

  piece->enabled = pipe->want_segmentation;
  piece->process_cl_ready = FALSE;
}

void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  tiling->maxbuf = 1.0f;
  tiling->xalign = 1;
  tiling->yalign = 1;
  tiling->overhead = 0;  // following have to be according to the chosen algorithm
  tiling->factor = 5.0f;
}

/*
void reload_defaults(dt_iop_module_t *self)
{
//  if(!self->dev || !dt_is_valid_imgid(self->dev->image_storage.id)) return;

}
*/
void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  self->hide_enable_button = TRUE;
  self->default_enabled = TRUE;
}

/*
void gui_update(dt_iop_module_t *self)
{
  gui_changed(self, NULL, NULL);
}
*/
void gui_init(dt_iop_module_t *self)
{
  dt_iop_segmentation_gui_data_t *g = IOP_GUI_ALLOC(segmentation);

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->model = dt_bauhaus_combobox_from_params(self, "model");
  gtk_widget_set_tooltip_text(g->model, _("chosen AI model"));

  g->depth = dt_bauhaus_slider_from_params(self, "depth");
  gtk_widget_set_tooltip_text(g->depth, _("restrict maximum number of segments. effect depends on chosen model"));
  dt_bauhaus_slider_set_format(g->depth, "%");
  dt_bauhaus_slider_set_digits(g->depth, 0);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
