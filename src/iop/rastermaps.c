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

#ifdef _OPENMP
#include <omp.h>
#endif

DT_MODULE_INTROSPECTION(1, dt_iop_segmap_params_t)

typedef enum dt_iop_segmap_model_t
{
  DT_SEGMAP_MODEL_VARIANCE = 0,   // $DESCRIPTION: "local variance"
  DT_SEGMAP_MODELS,
} dt_iop_segmap_model_t;

/* For every defined model we require these statics
  - model_hash reflects an internal version hash, the model algorithm must change it
    in case of new internals like training data
  - model_depth defines if the depth parameter will be visible
  - model_level defines if the level parameter will be visible
  - model_fbutton defines visibility of a file button
  - model_file defines visibility of a file
  - _depth_help provides the tooltip for depth parameter
  - _level_help provides the tooltip for level parameter
  - _model_help provides the tooltip for the selected model
*/

static dt_hash_t model_hash[DT_SEGMAP_MODELS]   = { DT_INITHASH };
static gboolean model_depth[DT_SEGMAP_MODELS]   = { TRUE };
static gboolean model_level[DT_SEGMAP_MODELS]   = { TRUE };
static gboolean model_fbutton[DT_SEGMAP_MODELS] = { FALSE };
static gboolean model_file[DT_SEGMAP_MODELS]    = { FALSE };

static char *_depth_help(const int model)
{
  switch(model)
  {
    case DT_SEGMAP_MODEL_VARIANCE:  return _("circular radius of variance calculation");
    default:                        return _("unknown");
  }
}

static char *_level_help(const int model)
{
  switch(model)
  {
    case DT_SEGMAP_MODEL_VARIANCE:  return _("threshold of variance calculation");
    default:                        return _("unknown");
  }
}

static char *_model_help(const int model)
{
  switch(model)
  {
    case DT_SEGMAP_MODEL_VARIANCE:  return _("create local variance maps for each RGB channel");
    default:                        return _("unknown");
  }
}

#define UNDEFINED_MOUSE_SEGMENT -2
#define NO_MOUSE_SEGMENT -1
#define SEGMAP_MAXSEGMENTS 128
#define RASTERMAP_MAXFILE 2048

typedef struct dt_segmentation_t
{
  dt_pthread_mutex_t lock;          // all access to segmentation data is done in locked state
  dt_hash_t hash;                   // the piece parameters hash
  dt_iop_segmap_model_t model;      // The UI mode requires this to avoid superfluos actions
  int segments;                     // provided segment maps after the segmentation
  int width, height;                // dimension of each segment map
  int threshold;                    // relevance threshold
  uint8_t *map[SEGMAP_MAXSEGMENTS]; // a map per segment

  /* After scaling the map to rastermask we might do some extra work like deblurring.
      If undefined _postprocess_default() is used.
      If defined make sure the mask data are in 0->1 range
  */
  void(*postprocess) (float *mask, int width, int height, int depth, int level);
} dt_segmentation_t;

typedef struct dt_iop_segmap_params_t
{
  dt_iop_segmap_model_t model;    // $DEFAULT: DT_SEGMAP_MODEL_VARIANCE $DESCRIPTION: "model"
  int depth;                      // $MIN: 0 $MAX: 20 $DEFAULT: 2 $DESCRIPTION: "model depth"
  int level;                      // $MIN: 0 $MAX: 20 $DEFAULT: 2 $DESCRIPTION: "model detail"
  uint8_t id[SEGMAP_MAXSEGMENTS];
  char path[RASTERMAP_MAXFILE];
  char file[RASTERMAP_MAXFILE];
} dt_iop_segmap_params_t;

typedef struct dt_iop_segmap_data_t
{
  dt_iop_segmap_model_t model;
  int depth;
  int level;
  uint8_t id[SEGMAP_MAXSEGMENTS];
} dt_iop_segmap_data_t;

typedef struct dt_iop_segmap_module_data_t
{
  dt_segmentation_t *segment;
} dt_iop_segmap_module_data_t;

const char *name()
{
  return _("segment maps");
}

const char *aliases()
{
  return _("segmentation|raster|mask|map|AI");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
      _("create and handle segment rastermasks"),
      _("corrective or creative"),
      _("linear, raw, scene-referred"),
      _("linear, raw"),
      _("linear, raw, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_WRITE_RASTER;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return (pipe && !dt_image_is_raw(&pipe->image)) ? IOP_CS_RGB : IOP_CS_RAW;
}

typedef struct dt_iop_segmap_gui_data_t
{
  GtkWidget *model;
  GtkWidget *depth;
  GtkWidget *level;
  GtkWidget *fbutton;
  GtkWidget *file;
  int mouse_segment;
  gboolean down;
  gboolean dclick;
} dt_iop_segmap_gui_data_t;

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  return 1;
}

void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *const roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
  roi_in->scale = 1.0f;
  roi_in->x = 0;
  roi_in->y = 0;
  roi_in->width = piece->buf_in.width;
  roi_in->height = piece->buf_in.height;
}

// the default postprocess algorithm, some blurring for edges plus range limit safety.
static void _postprocess_default(float *mask, const int width, const int height, const int depth, const int level)
{
  const float sigma = 1.0f;
  const float mmax[] = { 1.0f };
  const float mmin[] = { 0.0f };
  dt_gaussian_t *g = dt_gaussian_init(width, height, 1, mmax, mmin, sigma, 0);
  if(g)
  {
    dt_gaussian_blur(g, mask, mask);
    dt_gaussian_free(g);
  }
}

static void _variance_segment(float *in,
                              dt_segmentation_t *seg,
                              const int depth,
                              const int level,
                              const dt_iop_roi_t *roi)
{
  /*  For many algorithms we might want to scale down for performance reasons, in addition
      to that we might require some blurring or other preprocessing.
      As the stored uint8_t maps are later bilinear interpolated when inserted into the pipe
      we can effectively choose any size/ratio for the maps.
  */
  const int width = roi->width / 2;
  const int height = roi->height / 2;
  float *rgb = dt_iop_image_alloc(width, height, 4);
  if(!rgb)
  {
    dt_print(DT_DEBUG_ALWAYS, "can't provide variance segments because of low memory");
    dt_control_log(_("can't provide variance segments because of low memory"));
    return;
  }

  interpolate_bilinear(in, roi->width, roi->height, rgb, width, height, 4);
  seg->postprocess = NULL;
  seg->width = width;
  seg->height = height;
  seg->segments = 3;  // for many algorithms the number of presented segments will depend on depth
  seg->threshold = 4;
  for(int i = 0; i < seg->segments; i++)
    seg->map[i] = dt_calloc_align_type(uint8_t, (size_t)width * height);

  const int r = depth+1;
  const int limit = r * r + 1;
  const float power = 0.4f + 0.025f * level;

  DT_OMP_FOR()
  for(ssize_t row = 0; row < height; row++)
  {
    for(ssize_t col = 0; col < width; col++)
    {
      float pix = 0.0f; // count the pixels inside the circle
      dt_aligned_pixel_t av = { 0.0f, 0.0f, 0.0f, 0.0f };
      for(int y = MAX(0, row-r); y < MIN(height, row+r+1); y++)
      {
        for(int x = MAX(0, col-r); x < MIN(width, col+r+1); x++)
        {
          const int dx = x - col;
          const int dy = y - row;
          if((dx*dx + dy*dy) <= limit)
          {
            for_each_channel(c) av[c] += rgb[(size_t)4*(y*width + x) + c];
            pix += 1.0f;
          }
        }
      }
      for_each_channel(c) av[c] /= pix;

      dt_aligned_pixel_t sv = { 0.0f, 0.0f, 0.0f, 0.0f };
      for(int y = MAX(0, row-r); y < MIN(height, row+r+1); y++)
      {
        for(int x = MAX(0, col-r); x < MIN(width, col+r+1); x++)
        {
          const int dx = x - col;
          const int dy = y - row;
          if((dx*dx + dy*dy) <= limit)
          {
            for_each_channel(c) sv[c] += sqrf(rgb[(size_t)4*(y*width + x) + c] - av[c]);
          }
        }
      }
      for_each_channel(c) sv[c] /= (pix - 1.0f);

      for_three_channels(c)
        if(seg->map[c]) seg->map[c][row*width + col] = CLIP(3.0f * powf(sv[c], power)) * 255.0f;
    }
  }
  dt_print(DT_DEBUG_PIPE, "%d variance segments %dx%d provided hash=%"PRIx64, seg->segments, seg->width, seg->height, seg->hash);
  dt_control_log(_("%d variance segments %dx%d provided"), seg->segments, seg->width, seg->height);
  dt_free_align(rgb);
}

static float *_dev_get_segmentation_mask(dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_module_t *self = piece->module;
  dt_iop_segmap_data_t *d = piece->data;
  dt_iop_segmap_module_data_t *md = self->data;
  const dt_iop_roi_t *roi = &piece->processed_roi_in;
  const dt_iop_roi_t *roo = &piece->processed_roi_out;

  dt_segmentation_t *seg = md->segment;
  if(!seg) return NULL;

  float *src = dt_iop_image_alloc(seg->width, seg->height, 1);
  if(!src) return NULL;

  float *tmp = NULL;
  float *res = NULL;
  const uint8_t *cmap = d->id;
  DT_OMP_FOR()
  for(size_t k = 0; k < (size_t)seg->width * seg->height; k++)
  {
    uint8_t val = 0;
    for(int c = 0; c < seg->segments; c++)
    {
      if(cmap[c] && seg->map[c]) val = MAX(val, seg->map[c][k]);
    }
    src[k] = (float)val / 255.0f;
  }

  tmp = dt_iop_image_alloc(roi->width, roi->height, 1);
  if(!tmp) goto final;

  interpolate_bilinear(src, seg->width, seg->height, tmp, roi->width, roi->height, 1);

  if(seg->postprocess)
    seg->postprocess(tmp, roi->width, roi->height, d->depth, d->level);
  else
    _postprocess_default(tmp, roi->width, roi->height, d->depth, d->level);

  dt_free_align(src);
  src = NULL;

  res = dt_iop_image_alloc(roo->width, roo->height, 1);;
  if(res) self->distort_mask(self, piece, tmp, res, roi, roo);

final:
  dt_free_align(src);
  dt_free_align(tmp);
  return res;
}

static inline void _clean_segment(dt_segmentation_t *seg)
{
  for(int s = 0; s < SEGMAP_MAXSEGMENTS; s++)
  {
    dt_free_align(seg->map[s]);
    seg->map[s] = NULL;
  }
  seg->segments = seg->width = seg->height = seg->threshold = 0;
  seg->postprocess = NULL;
  seg->hash = DT_INVALID_HASH;
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_dev_pixelpipe_t *pipe = piece->pipe;
  const ssize_t ch = pipe->dsc.filters ? 1 : 4;
  const gboolean fullpipe = pipe->type & DT_DEV_PIXELPIPE_FULL;
  dt_iop_segmap_data_t *d = piece->data;
  const dt_hash_t hash = dt_hash(dt_dev_pixelpipe_piece_hash(piece, NULL, TRUE), &model_hash[d->model], sizeof(dt_hash_t));
  const gboolean visual = fullpipe && dt_iop_has_focus(self);
  dt_iop_segmap_module_data_t *md = piece->module->data;
  dt_segmentation_t *seg = md->segment;

  dt_pthread_mutex_lock(&seg->lock);
  const gboolean bad_hash = hash != seg->hash;
  dt_pthread_mutex_unlock(&seg->lock);
  const int devid = pipe->devid;
  cl_int err = DT_OPENCL_PROCESS_CL;

  if(visual || bad_hash)
  {
    dt_print_pipe(DT_DEBUG_PIPE, bad_hash ? "rastermap hash BAD" : "rastermap hash GOOD",
      pipe, self, devid, NULL, NULL, "piece hash=%"PRIx64" seg hash=%"PRIx64" CPU%s fallback",
      hash, seg->hash, visual ? "visualizing " : "");
    return err;
  }

  if(roi_out->scale != roi_in->scale && ch == 4)
    err = dt_iop_clip_and_zoom_roi_cl(devid, dev_out, dev_in, roi_out, roi_in);
  else
  {
    size_t iorigin[] = { roi_out->x, roi_out->y, 0 };
    size_t oorigin[] = { 0, 0, 0 };
    size_t region[] = { roi_out->width, roi_out->height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, iorigin, oorigin, region);
  }

  if(dt_iop_is_raster_mask_used(piece->module, BLEND_RASTER_ID))
  {
    float *mask = _dev_get_segmentation_mask(piece);
    dt_iop_piece_set_raster(piece, mask, roi_in, roi_out);
  }
  else
    dt_iop_piece_clear_raster(piece, NULL);

  return err;
}
#endif

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const float *const in = (float *)ivoid;
  float *const out = (float *)ovoid;
  dt_dev_pixelpipe_t *pipe = piece->pipe;
  const uint32_t filters = pipe->dsc.filters;
  const ssize_t ch = filters ? 1 : 4;

  if(roi_out->scale != roi_in->scale && ch == 4)
  {
    const dt_interpolation_t *itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
    dt_interpolation_resample(itor, out, roi_out, in, roi_in);
  }
  else
    dt_iop_copy_image_roi(out, in, ch, roi_in, roi_out);

  dt_iop_segmap_data_t *d = piece->data;
  const dt_iop_segmap_gui_data_t *g = self->gui_data;
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])pipe->dsc.xtrans;
  const gboolean fullpipe = pipe->type & DT_DEV_PIXELPIPE_FULL;
  const dt_hash_t hash = dt_hash(dt_dev_pixelpipe_piece_hash(piece, NULL, TRUE), &model_hash[d->model], sizeof(dt_hash_t));
  const gboolean is_xtrans = filters == 9u;
  const gboolean is_bayer = !is_xtrans && filters != 0;
  const gboolean request = dt_iop_is_raster_mask_used(piece->module, BLEND_RASTER_ID);
  const gboolean visual = fullpipe && dt_iop_has_focus(self);
  dt_iop_segmap_module_data_t *md = piece->module->data;
  dt_segmentation_t *seg = md->segment;

  dt_pthread_mutex_lock(&seg->lock);
  const gboolean bad_hash = hash != seg->hash;
  dt_print_pipe(DT_DEBUG_PIPE, bad_hash ? "rastermap hash BAD" : "rastermap hash GOOD",
    pipe, self, DT_DEVICE_NONE, NULL, NULL, "piece hash=%"PRIx64"  seg hash=%"PRIx64,
    hash, seg->hash);
  if(bad_hash) dt_iop_piece_clear_raster(piece, NULL);

  const gboolean provider = bad_hash && (pipe->type & (DT_DEV_PIXELPIPE_FULL | DT_DEV_PIXELPIPE_EXPORT));
  float *tmp = provider || visual ? dt_iop_image_alloc(roi_in->width, roi_in->height, 4) : NULL;

  if(provider)
  {
    _clean_segment(seg);
    if(tmp)
    {
      if(is_xtrans)
        dt_iop_clip_and_zoom_demosaic_third_size_xtrans_f(tmp, in, roi_in, roi_in, roi_in->width, roi_in->width, xtrans);
      else if(is_bayer)
        dt_iop_clip_and_zoom_demosaic_half_size_f(tmp, in, roi_in, roi_in, roi_in->width, roi_in->width, filters);

      dt_iop_image_scaled_copy(tmp, filters ? tmp : in, 1.0f / dt_iop_get_processed_maximum(piece), roi_in->width, roi_in->height, 4);

      seg->hash = hash;
      seg->model = d->model;
      seg->postprocess = NULL;
      /* This is where any segmentation takes place. We do this within a locked seg struct.

        We provide the RGB input data (float *tmp) normalized to 0->1,
          it's dimension (roi_in->width/height) and a desired segmentation depth and level.
        The meaning of depth and level depend on the model,
          for segmentation  algorithms lower values should lead to less segments and detail,
          other tools might use it otherwise.

        All algorithms *must* provide and set
          - the dimension of the sementation maps.
            Please note that you might chose a different aspect and downscaled input data
            for the algorithm performance.
          - a uint8_t map for every generated segment with above dimension.
            The selected combination of these maps is
            - first bilinear scaled to a full image mask and then
            - distorted by all modules module->distort_mask functions up to target module
                as requested by a raster mask receiving module.
          - the number of provided segments
          - possibly a threshold value used when in visualizing mode

        An **optional** postprocess function might be provided to correct problems resulting from
          the uin8_t maps or scaling.
      */
      switch(seg->model)
      {

        default: _variance_segment(tmp, seg, d->depth, d->level, roi_in);
      }
      if(!visual) dt_free_align(tmp);
    }
    else
    {
      dt_print(DT_DEBUG_ALWAYS, "can't provide segmentation because of low memory");
      dt_control_log(_("can't provide %d model segmentation because of low memory"), d->model);
    }
  }

  if(visual)
  {
    pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
    const uint8_t *mouse_map = g->mouse_segment > NO_MOUSE_SEGMENT ? seg->map[g->mouse_segment] : NULL;
    const uint8_t *cmap = d->id;

    const ssize_t owidth = roi_out->width;
    const ssize_t oheight = roi_out->height;
    const ssize_t iwidth = roi_in->width;
    const ssize_t iheight = roi_in->height;

    if(ch == 1)
    {
      dt_iop_image_copy(tmp, out, owidth * oheight);
      dt_box_mean(tmp, oheight, owidth, 1, 3, 2); // simple blur to remove CFA colors
      DT_OMP_FOR()
      for(ssize_t row = 0; row < oheight; row++)
      {
        for(ssize_t col = 0; col < owidth; col++)
        {
          const ssize_t k = owidth * row + col;
          const ssize_t irow = row + roi_out->y - roi_in->y;
          const ssize_t icol = col + roi_out->x - roi_in->x;
          if(irow < iheight && icol < iwidth && icol >= 0 && irow >= 0)
          {
            const ssize_t srow = irow * (ssize_t)seg->height / iheight;
            const ssize_t scol = icol * (ssize_t)seg->width / iwidth;
            const ssize_t sk = srow * (ssize_t)seg->width + scol;

            out[k] = 0.4f * CLAMPF(sqrtf(tmp[k]), 0.0f, 0.5f);
            const int color = is_xtrans ? FCxtrans(irow, icol, roi_in, xtrans) : FC(irow, icol, filters);
            /*  1. Brighten every location that has at least one segment
                2. If the mouse is over a segment, all segment locations are shown red
                3. The combination of all selected segments is shown green
                Note1: As we might have segment mask data with a mask value below a threshold
                        those are not visualized & tested.
                Note2: We might do better via a false-color map ?
            */

            for(int c = 0; c < seg->segments; c++)
            {
              if(seg->map[c] && seg->map[c][sk] > seg->threshold)
              {
                out[k] += 0.3f;
                break;
              }
            }

            if(color == 0 && mouse_map && mouse_map[sk] > seg->threshold)
              out[k] += 1.0f;

            if(color == 1)
            {
              for(int c = 0; c < seg->segments; c++)
              {
                if(cmap[c] && seg->map[c] && seg->map[c][sk] > seg->threshold)
                {
                  out[k] += 1.0f;
                  break;
                }
              }
            }
          }
        }
      }
    }
    else // 4 channels
    {
      DT_OMP_FOR()
      for(ssize_t row = 0; row < iheight; row++)
      {
        for(ssize_t col = 0; col < iwidth; col++)
        {
          const ssize_t k = ch * (iwidth * row + col);
          const ssize_t srow = row * (ssize_t)seg->height / iheight;
          const ssize_t scol = col * (ssize_t)seg->width / iwidth;
          const ssize_t sk = srow * (ssize_t)seg->width + scol;

          tmp[k] = tmp[k+1] = tmp[k+2] = 0.4f * CLAMPF(sqrtf(0.33f * (in[k] + in[k+1]+ in[k+2])), 0.0f, 0.5f);
          for(int c = 0; c < seg->segments; c++)
          {
            if(seg->map[c] && seg->map[c][sk] > seg->threshold)
            {
              for_three_channels(m) tmp[k+m] += 0.3f;
              break;
            }
          }

          if(mouse_map && mouse_map[sk] > seg->threshold)
            tmp[k] += 1.0f;

          for(int c = 0; c < seg->segments; c++)
          {
            if(cmap[c] && seg->map[c] && seg->map[c][sk] > seg->threshold)
            {
              tmp[k+1] += 1.0f;
              break;
            }
          }
        }
      }
      if(roi_out->scale != roi_in->scale)
      {
        const dt_interpolation_t *itor = dt_interpolation_new(DT_INTERPOLATION_BILINEAR);
        dt_interpolation_resample(itor, out, roi_out, tmp, roi_in);
      }
      else
        dt_iop_copy_image_roi(out, tmp, ch, roi_in, roi_out);
    }
    dt_free_align(tmp);
  }
  else // we are not in UI mode so we must update the raster mask
  {
    if(request)
    {
      float *mask = _dev_get_segmentation_mask(piece);
      dt_iop_piece_set_raster(piece, mask, roi_in, roi_out);
    }
    else
      dt_iop_piece_clear_raster(piece, NULL);

    if(fullpipe && provider) dt_dev_reprocess_preview(self->dev);
  }
  dt_pthread_mutex_unlock(&seg->lock);
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_segmap_params_t *p = (dt_iop_segmap_params_t *)p1;
  dt_iop_segmap_data_t *d = piece->data;

  d->depth = p->depth;
  d->level = p->level;
  d->model = p->model;
  memcpy(d->id, p->id, sizeof(uint8_t) * SEGMAP_MAXSEGMENTS);
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
  tiling->factor = 4.0f;
}

void reload_defaults(dt_iop_module_t *self)
{
  // we might be called from presets update infrastructure => there is no image
  if(!self->dev || !dt_is_valid_imgid(self->dev->image_storage.id)) return;

  self->default_enabled = FALSE;
  dt_iop_segmap_params_t *d = self->default_params;
  memset(d->id, 0, sizeof(uint8_t) * SEGMAP_MAXSEGMENTS);
  memset(d->path, 0, sizeof(char) * RASTERMAP_MAXFILE);
  memset(d->file, 0, sizeof(char) * RASTERMAP_MAXFILE);
}

void distort_mask(dt_iop_module_t *self,
                  dt_dev_pixelpipe_iop_t *piece,
                  const float *const in,
                  float *const out,
                  const dt_iop_roi_t *const roi_in,
                  const dt_iop_roi_t *const roi_out)
{
  if(roi_out->scale != roi_in->scale)
  {
    const dt_interpolation_t *itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
    dt_interpolation_resample_1c(itor, out, roi_out, in, roi_in);
  }
  else
    dt_iop_copy_image_roi(out, in, 1, roi_in, roi_out);
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_segmap_gui_data_t *g = self->gui_data;
  dt_iop_segmap_params_t *p = self->params;
  g->mouse_segment = UNDEFINED_MOUSE_SEGMENT;

  if(!w || w == g->model)
  {
    gtk_widget_set_tooltip_text(g->depth, _depth_help(p->model));
    gtk_widget_set_visible(g->depth, model_depth[p->model]);

    gtk_widget_set_tooltip_text(g->level, _level_help(p->model));
    gtk_widget_set_visible(g->level, model_level[p->model]);

    gtk_widget_set_tooltip_text(g->model, _model_help(p->model));

    if(g->fbutton) gtk_widget_set_visible(g->fbutton, model_fbutton[p->model]);
    if(g->file) gtk_widget_set_visible(g->file, model_file[p->model]);
  }
  if(!w)
    dt_dev_reprocess_center(self->dev);
}

void gui_update(dt_iop_module_t *self)
{
  gui_changed(self, NULL, NULL);
}

void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);

  dt_iop_segmap_params_t *d = self->default_params;
  memset(d->id, 0, sizeof(uint8_t) * SEGMAP_MAXSEGMENTS);
  memset(d->path, 0, sizeof(char) * RASTERMAP_MAXFILE);
  memset(d->file, 0, sizeof(char) * RASTERMAP_MAXFILE);

  dt_iop_segmap_module_data_t *md = calloc(1, sizeof(dt_iop_segmap_module_data_t));
  dt_segmentation_t *seg = calloc(1, sizeof(dt_segmentation_t));
  seg->hash = DT_INVALID_HASH;
  dt_pthread_mutex_init(&seg->lock, NULL);
  md->segment = seg;
  self->data = md;
}

void cleanup(dt_iop_module_t *self)
{
  dt_iop_default_cleanup(self);

  dt_iop_segmap_module_data_t *md = self->data;
  dt_segmentation_t *seg = md->segment;
  _clean_segment(seg);
  dt_pthread_mutex_destroy(&seg->lock);
  free(seg);
  free(md);
  self->data = NULL;
}

static void _mouse_update(dt_iop_module_t *self)
{
  dt_dev_invalidate(self->dev);
  dt_control_queue_redraw_center();
}

static inline size_t _get_seg_k(dt_iop_module_t *self,
                                dt_segmentation_t *seg,
                                const float x,
                                const float y)
{
  dt_develop_t *dev = self->dev;
  dt_dev_pixelpipe_t *fpipe = dev->full.pipe;

  /* slightly more complicated than usual as we calculate maps from data provided after rawprepare
      and scale to dimensions after that module.
  */
  const int rp_order = dt_ioppr_get_iop_order(self->dev->iop_order_list, "rawprepare", 0);
  float pts[2]= { x * (float)fpipe->processed_width, y * (float)fpipe->processed_height };
  dt_dev_distort_backtransform_plus(dev, fpipe, rp_order, DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, 1);

  const size_t sx = roundf((float)seg->width * CLIP(pts[0] / (float)dev->image_storage.p_width));
  const size_t sy = roundf((float)seg->height * CLIP(pts[1] / (float)dev->image_storage.p_height));
  return sy * seg->width + sx;
}

int mouse_moved(dt_iop_module_t *self, float x, float y, double pressure, int which, float zoom_scale)
{
  if(darktable.gui->reset) return FALSE;
  dt_iop_segmap_gui_data_t *g = self->gui_data;
  dt_iop_segmap_module_data_t *md = self->data;
  dt_segmentation_t *seg = md->segment;

  if(g->down || !dt_iop_has_focus(self) || !seg || x < 0.0f || y < 0.0f || y > 1.0f || x > 1.0f)
    return FALSE;

  gboolean other = g->mouse_segment == UNDEFINED_MOUSE_SEGMENT;

  dt_pthread_mutex_lock(&seg->lock);
  const gboolean available = seg->segments > 0;
  if(available)
  {
    int over = NO_MOUSE_SEGMENT;
    const size_t k = _get_seg_k(self, seg, x, y);
    for(int s = 0; s < seg->segments; s++)
    {
      if(seg->map[s] && seg->map[s][k] > seg->threshold)
      {
        over = s;
        break;
      }
    }

    if(g->mouse_segment != over)
    {
      g->mouse_segment = over;
      other = TRUE;
    }
  }
  dt_pthread_mutex_unlock(&seg->lock);

  if(available && other)
    _mouse_update(self);
  return TRUE;
}

int button_pressed(dt_iop_module_t *self, const float x, const float y, const double pressure,
                   const int which, const int type, const uint32_t state, const float zoom_scale)
{
  if(darktable.gui->reset) return FALSE;
  dt_iop_segmap_gui_data_t *g = self->gui_data;
  g->down = TRUE;
  // keep track about double clicks
  g->dclick = (type == GDK_DOUBLE_BUTTON_PRESS);
  return TRUE;
}

int button_released(dt_iop_module_t *self, float x, float y, int which, uint32_t state, float zoom_scale)
{
  if(darktable.gui->reset) return FALSE;
  dt_iop_segmap_gui_data_t *g = self->gui_data;
  dt_iop_segmap_params_t *p = self->params;
  g->down = FALSE;

  // A double click while being in UI visualizing mode will unfocus to keep darkroom behaviour
  if(g->dclick && dt_iop_has_focus(self))
  {
    dt_iop_request_focus(NULL);
    return TRUE;
  }

  dt_iop_segmap_module_data_t *md = self->data;
  dt_segmentation_t *seg = md->segment;

  /* we only accept single left or right button clicks with shift or nothing mode
      and make sure we have focus and valid positions
  */
  if(!seg || g->dclick || !dt_iop_has_focus(self) || x < 0.0f || y < 0.0f || y > 1.0f || x > 1.0f)
    return FALSE;
  if(!(which == GDK_BUTTON_PRIMARY || which == GDK_BUTTON_SECONDARY))
    return FALSE;
  if(state & ~GDK_SHIFT_MASK)
    return FALSE;

  gboolean update = FALSE;
  if(state == GDK_SHIFT_MASK)
  {
    memset(p->id, which == GDK_BUTTON_PRIMARY ? 1 : 0, sizeof(uint8_t) * SEGMAP_MAXSEGMENTS);
    update = TRUE;
  }
  else
  {
    dt_pthread_mutex_lock(&seg->lock);
    if(seg->segments > 0)
    {
      const size_t k = _get_seg_k(self, seg, x, y);
      if(which == GDK_BUTTON_PRIMARY)  // add first found *disabled* map to selection
      {
        for(int s = 0; s < seg->segments; s++)
        {
          if(p->id[s] == 0 && seg->map[s] && seg->map[s][k] > seg->threshold)
          {
            p->id[s] = 1;
            update = TRUE;
            break;
          }
        }
      }
      else  // remove the first found *enabled* map from selection
      {
        for(int s = 0; s < seg->segments; s++)
        {
          if(p->id[s] == 1 && seg->map[s] && seg->map[s][k] > seg->threshold)
          {
            p->id[s] = 0;
            update = TRUE;
            break;
          }
        }
      }
    }
    dt_pthread_mutex_unlock(&seg->lock);
  }

  if(update)
    dt_dev_add_history_item(darktable.develop, self, FALSE);
  return TRUE;
}

void gui_focus(dt_iop_module_t *self, gboolean in)
{
  dt_iop_segmap_gui_data_t *g = self->gui_data;
  g->mouse_segment = UNDEFINED_MOUSE_SEGMENT;
  dt_dev_reprocess_center(self->dev);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_segmap_gui_data_t *g = IOP_GUI_ALLOC(segmap);

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->model = dt_bauhaus_combobox_from_params(self, "model");
  g->depth = dt_bauhaus_slider_from_params(self, "depth");
  g->level = dt_bauhaus_slider_from_params(self, "level");
}

#undef UNDEFINED_MOUSE_SEGMENT
#undef NO_MOUSE_SEGMENT
#undef SEGMAP_MAXSEGMENTS
#undef RASTERMAP_MAXFILE

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
