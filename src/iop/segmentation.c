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
#include "common/interpolation.h"
#include "common/fast_guided_filter.h"
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
  dt_iop_segmentation_model_t model;  // $DEFAULT: DT_IOP_SEGMENTATION_MODEL_FELSENZWALB $DESCRIPTION: "model"
  int depth;                          // $MIN: 1 $MAX: SEGMENTATION_MAXSEGMENTS $DEFAULT: 4 $DESCRIPTION: "limitation"
  int raster;                         // $MIN: 0 $MAX: SEGMENTATION_MAXSEGMENTS $DEFAULT: 1 $DESCRIPTION: "advertised segment"
} dt_iop_segmentation_params_t;

typedef struct dt_iop_segmentation_data_t
{
  dt_iop_segmentation_model_t model;
  int depth;
  int raster;
} dt_iop_segmentation_data_t;

typedef struct dt_iop_segmentation_global_data_t
{
  dt_segmentation_t *global_segments[SEGMENTATION_INSTANCES];
  dt_iop_module_t *segmentizer[SEGMENTATION_INSTANCES];
} dt_iop_segmentation_global_data_t;

const char *name()
{
  return _("segmentation");
}

const char *aliases()
{
  return _("segmentation masks");
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
  return IOP_FLAGS_WRITE_SEGMENTATION | IOP_FLAGS_UNSAFE_COPY;
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
  GtkWidget *raster;
  gboolean masking;
} dt_iop_segmentation_gui_data_t;

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  return 1;
}


// just have this while implementing/debugging
static void _dummy_segmentation(dt_segmentation_t *seg,
                                int isegments,
                                const float *const in,
                                const dt_iop_roi_t *roi)
{
  /* for many algorithms we might want to scale down for performance reasons, in addition
     to that we might require some blurring or other preprocessing.
     As the stored uint8_t maps are later bilinear interpolated when inserted into the pipe
     we can effectively chose any size/ratio for the maps demonstrated here.
  */
  const int width = roi->width / 4;
  const int height = roi->height / 5;
  float *rgb = dt_alloc_align_float((size_t)width * height * 4);
  if(!rgb) return;
  interpolate_bilinear(in, roi->width, roi->height, rgb, width, height, 4);

  const int segments = MIN(isegments, 9);
  for(int i = 0; i < segments; i++)
    seg->map[i] = dt_calloc_align_type(uint8_t, (size_t)width * height);

  seg->width = width;
  seg->height = height;
  seg->segments = segments;

  // just do something that can be seen & used
  DT_OMP_FOR()
  for(size_t row = 0; row < height; row++)
  {
    for(size_t col = 0; col < width; col++)
    {
      const size_t i = (size_t)row * width + col;
      if((row < height/2) && (col < width/2) && seg->map[0]) seg->map[0][i] = 255;
      if((row > height/2) && (col > width/2) && seg->map[1]) seg->map[1][i] = 255;
      if((row > height/2) && (col < width/2) && seg->map[2]) seg->map[2][i] = 255;
      if((row < height/2) && (col > width/2) && seg->map[3]) seg->map[3][i] = 255;
      if(((row < height/4)||(row > height* 3/4)) && ((col < width/4) || (col > width*3/4)) && seg->map[4]) seg->map[4][i] = 255;
      if(rgb[4*i]   > 0.2f && seg->map[5]) seg->map[5][i] = 255;
      if(rgb[4*i+1] > 0.2f && seg->map[6]) seg->map[6][i] = 255;
      if(rgb[4*i+2] > 0.2f && seg->map[7]) seg->map[7][i] = 255;
      if(rgb[4*i] > 0.8f && rgb[4*i+1] > 0.8f && rgb[4*i+2] > 0.8f && seg->map[8]) seg->map[8][i] = 255;
    }
  }
  dt_free_align(rgb);
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

float *_dev_get_segment(dt_dev_pixelpipe_iop_t *piece,
                        dt_segmentation_t *seg,
                        const uint32_t segment)
{
  uint8_t *map = (segment < seg->segments) ? seg->map[segment] : NULL;
  float *out = map ? dt_alloc_align_float((size_t)seg->width * seg->height) : NULL;
  if(out)
  {
    DT_OMP_FOR()
    for(size_t k = 0; k < (size_t)seg->width * seg->height; k++)
      out[k] = (float)map[k] / 255.0f;
  }
  return out;
}

static inline gboolean _skip_piece_on_tags(const dt_dev_pixelpipe_iop_t *piece)
{
  if(!piece->enabled) return TRUE;
  return dt_iop_module_is_skipped(piece->module->dev, piece->module)
          && (piece->pipe->type & DT_DEV_PIXELPIPE_BASIC);
}

static float *_dev_get_segmentation_mask(dt_dev_pixelpipe_iop_t *piece,
                                         const dt_iop_module_t *target_module,
                                         const uint32_t instance,
                                         const uint32_t segment)
{
  dt_iop_module_so_t *segmentizers = darktable.develop->segmentizers;
  dt_iop_segmentation_global_data_t *gd = segmentizers ? segmentizers->data : NULL;

  if(!gd || instance >= SEGMENTATION_INSTANCES) return NULL;

  dt_segmentation_t *seg = gd->global_segments[instance];
  dt_iop_module_t *segmentizer = gd->segmentizer[instance];

  if(!seg || !segmentizer) return NULL;

  dt_pthread_mutex_lock(&seg->lock);
  float *src = _dev_get_segment(piece, seg, segment);
  const int swidth = seg->width;
  const int sheight = seg->height;
  dt_pthread_mutex_unlock(&seg->lock);
  if(!src) return NULL;

  float *resmask = src;
  float *inmask  = src;
  dt_iop_roi_t *final_roi = &piece->processed_roi_in;

  GList *iter;
  for(iter = piece->pipe->nodes; iter; iter = g_list_next(iter))
  {
    dt_dev_pixelpipe_iop_t *it_piece = iter->data;
    if(!_skip_piece_on_tags(it_piece))
    {
      if(it_piece->module->distort_mask)
      {
        const int owidth = it_piece->processed_roi_out.width;
        const int oheight = it_piece->processed_roi_out.height;
        dt_print_pipe(DT_DEBUG_MASKS | DT_DEBUG_PIPE | DT_DEBUG_VERBOSE,
             "distort segmentation mask", piece->pipe, it_piece->module, DT_DEVICE_NONE, &it_piece->processed_roi_in, &it_piece->processed_roi_out);
        float *tmp = dt_alloc_align_float((size_t)owidth * oheight);
        if(dt_iop_module_is(it_piece->module->so, "rawprepare"))
        {
          float *gt = dt_alloc_align_float((size_t)owidth * oheight);
          interpolate_bilinear(inmask, swidth, sheight, gt, owidth, oheight, 1);
          dt_gaussian_fast_blur(gt, tmp, owidth, oheight, 2.0f, 0.0f, 1.0f, 1);
          dt_free_align(gt);
        }
        else if(!(dt_iop_module_is(it_piece->module->so, "finalscale")
                  && it_piece->processed_roi_in.width == 0
                  && it_piece->processed_roi_in.height == 0))
        {
          it_piece->module->distort_mask(it_piece->module, it_piece, inmask, tmp, &it_piece->processed_roi_in, &it_piece->processed_roi_out);
        }
        resmask = tmp;
        if(inmask != src) dt_free_align(inmask);
        inmask = tmp;
        final_roi = &it_piece->processed_roi_out;
      }
      if(it_piece->module == target_module) break;
    }
  }
  const gboolean correct = piece->processed_roi_out.width == final_roi->width
                        && piece->processed_roi_out.height == final_roi->height;

  dt_print_pipe(DT_DEBUG_MASKS | DT_DEBUG_PIPE,
      correct ? "got segment mask" : "SEGMENT SIZE MISMATCH",
       piece->pipe, target_module, DT_DEVICE_NONE, NULL, &piece->processed_roi_out,
        "%ix%i", final_roi->width, final_roi->height);

  if(!correct)
  {
    dt_free_align(resmask);
    resmask = NULL;
  }
  return resmask;
}

static inline void _clean_segment(dt_segmentation_t *seg)
{
  for(int i = 0; i < SEGMENTATION_MAXSEGMENTS; i++)
  {
    dt_free_align(seg->map[i]);
    seg->map[i] = NULL;
  }
  seg->segments = seg->width = seg->height = 0;
  seg->hash = DT_INVALID_CACHEHASH;
}

static inline void _restart_pipe(dt_dev_pixelpipe_t *pipe, dt_iop_module_t *self)
{
  dt_atomic_set_int(&pipe->shutdown, self->iop_order);
  pipe->changed |= DT_DEV_PIPE_SYNCH;
}

static gboolean restarted = FALSE;
void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(piece->colors != 4) return;
  const float *const in = (float *)ivoid;
  float *const out = (float *)ovoid;
  dt_iop_image_copy(out, in, (size_t)roi_in->width * roi_in->height * 4);

  const uint32_t instance = self->multi_priority;
  if(instance >= SEGMENTATION_INSTANCES)
  {
    dt_iop_set_module_trouble_message(self,
      _("high instance"), _("this module has a limited number of instances"), "high instance");
    return;
  }

  dt_dev_pixelpipe_t *pipe = piece->pipe;
  dt_iop_segmentation_data_t *d = piece->data;
  dt_iop_segmentation_gui_data_t *g = self->gui_data;

  // export or full pipes might generate the segmentation masks
  const gboolean provider = pipe->type & (DT_DEV_PIXELPIPE_FULL | DT_DEV_PIXELPIPE_EXPORT);
  const gboolean fullpipe = pipe->type & DT_DEV_PIXELPIPE_FULL;
  const dt_iop_segmentation_global_data_t *gd = self->so->data;

  dt_hash_t hash = dt_dev_pixelpipe_piece_hash(piece, NULL, FALSE);
  hash = dt_hash(hash, &d->model, sizeof(dt_iop_segmentation_model_t));
  hash = dt_hash(hash, &d->depth, sizeof(int));
  dt_segmentation_t *seg = gd->global_segments[instance];

  const int raster_id = d->raster -1;

  dt_pthread_mutex_lock(&seg->lock);
  const gboolean same_hash = hash == seg->hash;
  const gboolean has_mask = raster_id >= 0 && raster_id < seg->segments;
  if(same_hash)
  {
    dt_print_pipe(DT_DEBUG_PIPE, "segmentation available", pipe, self, DT_DEVICE_NONE, NULL, NULL,
        "instance=%i '%s`: %d segments %dx%d",
        instance, _algo_name(seg->model), seg->segments, seg->width, seg->height);
  }
  else
    g_hash_table_remove(piece->raster_masks, GINT_TO_POINTER(BLEND_RASTER_ID));

  dt_pthread_mutex_unlock(&seg->lock);

  if(!same_hash && provider)
  {
    if(!darktable.develop->late_scaling.enabled && fullpipe)
    {
      dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE, "HQ request", pipe, piece->module, DT_DEVICE_NONE, NULL, NULL);
      darktable.develop->late_scaling.enabled = TRUE;
      restarted = TRUE;
      _restart_pipe(pipe, self);
      return;
    }
    else
    {
      dt_pthread_mutex_lock(&seg->lock);
      _clean_segment(seg);
      seg->hash = hash;
      seg->model = d->model;

      // We can now process the AI segmentation algorithm, that must define the structs data
      switch(seg->model)
      {
        default: _dummy_segmentation(seg, d->depth, in, roi_in);
      }

      dt_print_pipe(DT_DEBUG_PIPE, "segmentation processed", pipe, self, DT_DEVICE_CPU, roi_in, NULL,
          "instance=%i '%s`: %d segments %dx%d",
          instance, _algo_name(seg->model), seg->segments, seg->width, seg->height);
      dt_pthread_mutex_unlock(&seg->lock);

      if(restarted)
      {
        dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE, "HQ done", pipe, piece->module, DT_DEVICE_NONE, NULL, NULL);
        darktable.develop->late_scaling.enabled = FALSE;
        restarted = FALSE;
        _restart_pipe(pipe, self);
        dt_dev_reprocess_preview(self->dev);
        return;
      }
    }
  }

  const gboolean visualize = g && g->masking && fullpipe;
  const gboolean announce = piece->pipe->store_all_raster_masks
                          || dt_iop_is_raster_mask_used(piece->module, BLEND_RASTER_ID);
  float *mask = ((visualize || announce) && has_mask)
        ? _dev_get_segmentation_mask(piece, self, instance, raster_id)
        : NULL;
  if(!mask)
  {
    g_hash_table_remove(piece->raster_masks, GINT_TO_POINTER(BLEND_RASTER_ID));
    return;
  }

  if(visualize)
  {
    pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
    DT_OMP_FOR()
    for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
    {
      const size_t i = k * 4;
      const float val = 0.5f * (0.3f * out[i] + 0.6f * out[i+1] + 0.1f * out[i+2]);
      out[i] = out[i+1] = val + (mask ? mask[k] : 0.0f);
      out[i+2] = val;
    }
  }
  if(announce)
    g_hash_table_replace(piece->raster_masks, GINT_TO_POINTER(BLEND_RASTER_ID), mask);
  else
    dt_free_align(mask);
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
  d->raster = p->raster;

  const char *name = (self->multi_priority > 0 || self->multi_name_hand_edited)
                    ? self->multi_name
                    : self->op;
  g_hash_table_remove_all(self->raster_mask.source.masks);
  g_hash_table_insert(self->raster_mask.source.masks,
                      GINT_TO_POINTER(BLEND_RASTER_ID), g_strdup(name));
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
  tiling->factor = 2.0f;
}

static void _quad_callback(GtkWidget *quad, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_segmentation_gui_data_t *g = self->gui_data;
  g->masking = dt_bauhaus_widget_get_quad_active(quad);
  dt_dev_reprocess_center(self->dev);
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_segmentation_gui_data_t *g = self->gui_data;
  if(!w || w != g->raster)
  {
    dt_bauhaus_widget_set_quad_active(g->raster, FALSE);
    g->masking = FALSE;
  }
  else if(w == g->raster)
    g->masking = dt_bauhaus_widget_get_quad_active(g->raster);
}

void gui_update(dt_iop_module_t *self)
{
  gui_changed(self, NULL, NULL);
  dt_dev_reprocess_center(self->dev);
}

void init(dt_iop_module_t *self)
{
  dt_iop_segmentation_global_data_t *gd = self->so->data;
  dt_iop_default_init(self);
  if(self->multi_priority < SEGMENTATION_INSTANCES)
    gd->segmentizer[self->multi_priority] = self;
}

void init_global(dt_iop_module_so_t *module)
{
  dt_iop_segmentation_global_data_t *gd = malloc(sizeof(dt_iop_segmentation_global_data_t));
  module->data = gd;
  for(int n = 0; n < SEGMENTATION_INSTANCES; n++)
  {
    dt_segmentation_t *seg = calloc(1, sizeof(dt_segmentation_t));
    for(int i = 0; i < SEGMENTATION_MAXSEGMENTS; i++)
      seg->map[i] = NULL;
    seg->segments = seg->width = seg->height = 0;
    seg->hash = DT_INVALID_CACHEHASH;
    dt_pthread_mutex_init(&seg->lock, NULL);
    gd->global_segments[n] = seg;
    gd->segmentizer[n] = NULL;
  }
  darktable.develop->segmentizers = module;

  if(darktable.tmp_directory == NULL)
    darktable.tmp_directory = g_dir_make_tmp("darktable_XXXXXX", NULL);
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_segmentation_global_data_t *gd = module->data;
  for(int i = 0; i < SEGMENTATION_INSTANCES; i++)
  {
    dt_segmentation_t *seg = gd->global_segments[i];
    _clean_segment(seg);
    dt_pthread_mutex_destroy(&seg->lock);
    free(seg);
    gd->segmentizer[i] = NULL;
  }
  free(gd);
  module->data = NULL;
}

void gui_focus(dt_iop_module_t *self, gboolean in)
{
  dt_iop_segmentation_gui_data_t *g = self->gui_data;
  if(!in)
  {
    if(g->masking)
    {
      g->masking = FALSE;
      dt_bauhaus_widget_set_quad_active(g->raster, FALSE);
      dt_dev_reprocess_center(self->dev);
    }
  }
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_segmentation_gui_data_t *g = IOP_GUI_ALLOC(segmentation);

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->model = dt_bauhaus_combobox_from_params(self, "model");
  gtk_widget_set_tooltip_text(g->model, _("chosen segmentation model"));

  g->depth = dt_bauhaus_slider_from_params(self, "depth");
  gtk_widget_set_tooltip_text(g->depth, _("restrict maximum number of segments. effect depends on chosen model"));

  g->raster = dt_bauhaus_slider_from_params(self, "raster");
  gtk_widget_set_tooltip_text(g->raster, _("chosen segment is advertised as raster mask, for 0 nothing is advertised"));
  dt_bauhaus_widget_set_quad(g->raster, self, dtgtk_cairo_paint_showmask, TRUE, _quad_callback,
    _("visualize chosen raster segment"));

}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
