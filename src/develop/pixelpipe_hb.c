/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2011 henrik andersson.
    copyright (c) 2016 Roman Lebedev.

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
#include "common/color_picker.h"
#include "common/colorspaces.h"
#include "common/histogram.h"
#include "common/imageio.h"
#include "common/opencl.h"
#include "control/control.h"
#include "control/signal.h"
#include "develop/blend.h"
#include "develop/format.h"
#include "develop/imageop_math.h"
#include "develop/pixelpipe.h"
#include "develop/tiling.h"
#include "develop/masks.h"
#include "gui/gtk.h"
#include "libs/colorpicker.h"
#include "libs/lib.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

typedef enum dt_pixelpipe_flow_t
{
  PIXELPIPE_FLOW_NONE = 0,
  PIXELPIPE_FLOW_HISTOGRAM_NONE = 1 << 0,
  PIXELPIPE_FLOW_HISTOGRAM_ON_CPU = 1 << 1,
  PIXELPIPE_FLOW_HISTOGRAM_ON_GPU = 1 << 2,
  PIXELPIPE_FLOW_PROCESSED_ON_CPU = 1 << 3,
  PIXELPIPE_FLOW_PROCESSED_ON_GPU = 1 << 4,
  PIXELPIPE_FLOW_PROCESSED_WITH_TILING = 1 << 5,
  PIXELPIPE_FLOW_BLENDED_ON_CPU = 1 << 6,
  PIXELPIPE_FLOW_BLENDED_ON_GPU = 1 << 7
} dt_pixelpipe_flow_t;

typedef enum dt_pixelpipe_picker_source_t
{
  PIXELPIPE_PICKER_INPUT = 0,
  PIXELPIPE_PICKER_OUTPUT = 1
} dt_pixelpipe_picker_source_t;

#include "develop/pixelpipe_cache.c"

static void get_output_format(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                              dt_develop_t *dev, dt_iop_buffer_dsc_t *dsc);

static char *_pipe_type_to_str(int pipe_type)
{
  char *r;

  switch(pipe_type)
  {
    case DT_DEV_PIXELPIPE_PREVIEW:
      r = "preview";
      break;
    case DT_DEV_PIXELPIPE_FULL:
      r = "full";
      break;
    case DT_DEV_PIXELPIPE_THUMBNAIL:
      r = "thumbnail";
      break;
    case DT_DEV_PIXELPIPE_EXPORT:
      r = "export";
      break;
    default:
      r = "unknown";
  }
  return r;
}

int dt_dev_pixelpipe_init_export(dt_dev_pixelpipe_t *pipe, int32_t width, int32_t height, int levels)
{
  int res = dt_dev_pixelpipe_init_cached(pipe, 4 * sizeof(float) * width * height, 2);
  pipe->type = DT_DEV_PIXELPIPE_EXPORT;
  pipe->levels = levels;
  return res;
}

int dt_dev_pixelpipe_init_thumbnail(dt_dev_pixelpipe_t *pipe, int32_t width, int32_t height)
{
  int res = dt_dev_pixelpipe_init_cached(pipe, 4 * sizeof(float) * width * height, 2);
  pipe->type = DT_DEV_PIXELPIPE_THUMBNAIL;
  return res;
}

int dt_dev_pixelpipe_init_dummy(dt_dev_pixelpipe_t *pipe, int32_t width, int32_t height)
{
  int res = dt_dev_pixelpipe_init_cached(pipe, 4 * sizeof(float) * width * height, 0);
  pipe->type = DT_DEV_PIXELPIPE_THUMBNAIL;
  return res;
}

int dt_dev_pixelpipe_init_preview(dt_dev_pixelpipe_t *pipe)
{
  // don't know which buffer size we're going to need, set to 0 (will be alloced on demand)
  int res = dt_dev_pixelpipe_init_cached(
      pipe, 0, 5);
  pipe->type = DT_DEV_PIXELPIPE_PREVIEW;
  return res;
}

int dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe)
{
  // don't know which buffer size we're going to need, set to 0 (will be alloced on demand)
  int res = dt_dev_pixelpipe_init_cached(
      pipe, 0, 5);
  pipe->type = DT_DEV_PIXELPIPE_FULL;
  return res;
}

int dt_dev_pixelpipe_init_cached(dt_dev_pixelpipe_t *pipe, size_t size, int32_t entries)
{
  pipe->devid = -1;
  pipe->changed = DT_DEV_PIPE_UNCHANGED;
  pipe->processed_width = pipe->backbuf_width = pipe->iwidth = 0;
  pipe->processed_height = pipe->backbuf_height = pipe->iheight = 0;
  pipe->nodes = NULL;
  pipe->backbuf_size = size;
  if(!dt_dev_pixelpipe_cache_init(&(pipe->cache), entries, pipe->backbuf_size)) return 0;
  pipe->cache_obsolete = 0;
  pipe->backbuf = NULL;
  pipe->processing = 0;
  pipe->shutdown = 0;
  pipe->opencl_error = 0;
  pipe->tiling = 0;
  pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  pipe->input_timestamp = 0;
  pipe->levels = IMAGEIO_RGB | IMAGEIO_INT8;
  dt_pthread_mutex_init(&(pipe->backbuf_mutex), NULL);
  dt_pthread_mutex_init(&(pipe->busy_mutex), NULL);
  pipe->icc_type = DT_COLORSPACE_NONE;
  pipe->icc_filename = NULL;
  pipe->icc_intent = DT_INTENT_LAST;
  pipe->iop = NULL;
  pipe->forms = NULL;

  return 1;
}

void dt_dev_pixelpipe_set_input(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, float *input, int width, int height,
                                float iscale)
{
  pipe->iwidth = width;
  pipe->iheight = height;
  pipe->iscale = iscale;
  pipe->input = input;
  pipe->image = dev->image_storage;
  get_output_format(NULL, pipe, NULL, dev, &pipe->dsc);
}

void dt_dev_pixelpipe_set_icc(dt_dev_pixelpipe_t *pipe, dt_colorspaces_color_profile_type_t icc_type,
                              const gchar *icc_filename, dt_iop_color_intent_t icc_intent)
{
  pipe->icc_type = icc_type;
  g_free(pipe->icc_filename);
  pipe->icc_filename = g_strdup(icc_filename?icc_filename:"");
  pipe->icc_intent = icc_intent;
}

void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe)
{
  dt_pthread_mutex_lock(&pipe->backbuf_mutex);
  pipe->backbuf = NULL;
  // blocks while busy and sets shutdown bit:
  dt_dev_pixelpipe_cleanup_nodes(pipe);
  // so now it's safe to clean up cache:
  dt_dev_pixelpipe_cache_cleanup(&(pipe->cache));
  dt_pthread_mutex_unlock(&pipe->backbuf_mutex);
  dt_pthread_mutex_destroy(&(pipe->backbuf_mutex));
  dt_pthread_mutex_destroy(&(pipe->busy_mutex));
  pipe->icc_type = DT_COLORSPACE_NONE;
  g_free(pipe->icc_filename);
  pipe->icc_filename = NULL;

  if (pipe->forms)
  {
    g_list_free_full(pipe->forms, (void (*)(void *))dt_masks_free_form);
    pipe->forms = NULL;
  }
}

void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe)
{
  // FIXME: either this or all process() -> gdk mutices have to be changed!
  //        (this is a circular dependency on busy_mutex and the gdk mutex)
  dt_pthread_mutex_lock(&pipe->busy_mutex);
  pipe->shutdown = 1;
  // destroy all nodes
  GList *nodes = pipe->nodes;
  while(nodes)
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    // printf("cleanup module `%s'\n", piece->module->name());
    piece->module->cleanup_pipe(piece->module, pipe, piece);
    free(piece->blendop_data);
    piece->blendop_data = NULL;
    free(piece->histogram);
    piece->histogram = NULL;
    free(piece);
    nodes = g_list_next(nodes);
  }
  g_list_free(pipe->nodes);
  pipe->nodes = NULL;
  // also cleanup iop here
  if(pipe->iop)
  {
    g_list_free(pipe->iop);
    pipe->iop = NULL;
  }
  dt_pthread_mutex_unlock(&pipe->busy_mutex);
}

void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex);
  pipe->shutdown = 0;
  g_assert(pipe->nodes == NULL);
  g_assert(pipe->iop == NULL);
  // for all modules in dev:
  pipe->iop = g_list_copy(dev->iop);
  GList *modules = pipe->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    // if(module->enabled) // no! always create nodes. just don't process.
    {
      dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)calloc(1, sizeof(dt_dev_pixelpipe_iop_t));
      piece->enabled = module->enabled;
      piece->request_histogram = DT_REQUEST_ONLY_IN_GUI;
      piece->histogram_params.roi = NULL;
      piece->histogram_params.bins_count = 256;
      piece->histogram_stats.bins_count = 0;
      piece->histogram_stats.pixels = 0;
      piece->colors
          = ((dt_iop_module_colorspace(module) == iop_cs_RAW) && (pipe->image.flags & DT_IMAGE_RAW)) ? 1 : 4;
      piece->iscale = pipe->iscale;
      piece->iwidth = pipe->iwidth;
      piece->iheight = pipe->iheight;
      piece->module = module;
      piece->pipe = pipe;
      piece->data = NULL;
      piece->hash = 0;
      piece->process_cl_ready = 0;
      piece->process_tiling_ready = 0;
      dt_iop_init_pipe(piece->module, pipe, piece);
      pipe->nodes = g_list_append(pipe->nodes, piece);
    }

    modules = g_list_next(modules);
  }
  dt_pthread_mutex_unlock(&pipe->busy_mutex);
}

// helper
void dt_dev_pixelpipe_synch(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, GList *history)
{
  dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
  // find piece in nodes list
  GList *nodes = pipe->nodes;
  dt_dev_pixelpipe_iop_t *piece = NULL;
  while(nodes)
  {
    piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    if(piece->module == hist->module)
    {
      piece->enabled = hist->enabled;
      dt_iop_commit_params(hist->module, hist->params, hist->blend_params, pipe, piece);
    }
    nodes = g_list_next(nodes);
  }
}

void dt_dev_pixelpipe_synch_all(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex);
  // call reset_params on all pieces first.
  GList *nodes = pipe->nodes;
  while(nodes)
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    piece->hash = 0;
    piece->enabled = piece->module->default_enabled;
    dt_iop_commit_params(piece->module, piece->module->default_params, piece->module->default_blendop_params,
                         pipe, piece);
    nodes = g_list_next(nodes);
  }
  // go through all history items and adjust params
  GList *history = dev->history;
  for(int k = 0; k < dev->history_end && history; k++)
  {
    dt_dev_pixelpipe_synch(pipe, dev, history);
    history = g_list_next(history);
  }
  dt_pthread_mutex_unlock(&pipe->busy_mutex);
}

void dt_dev_pixelpipe_synch_top(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex);
  GList *history = g_list_nth(dev->history, dev->history_end - 1);
  if(history) dt_dev_pixelpipe_synch(pipe, dev, history);
  dt_pthread_mutex_unlock(&pipe->busy_mutex);
}

void dt_dev_pixelpipe_change(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&dev->history_mutex);
  // case DT_DEV_PIPE_UNCHANGED: case DT_DEV_PIPE_ZOOMED:
  if(pipe->changed & DT_DEV_PIPE_TOP_CHANGED)
  {
    // only top history item changed.
    dt_dev_pixelpipe_synch_top(pipe, dev);
  }
  if(pipe->changed & DT_DEV_PIPE_SYNCH)
  {
    // pipeline topology remains intact, only change all params.
    dt_dev_pixelpipe_synch_all(pipe, dev);
  }
  if(pipe->changed & DT_DEV_PIPE_REMOVE)
  {
    // modules have been added in between or removed. need to rebuild the whole pipeline.
    dt_dev_pixelpipe_cleanup_nodes(pipe);
    dt_dev_pixelpipe_create_nodes(pipe, dev);
    dt_dev_pixelpipe_synch_all(pipe, dev);
  }
  pipe->changed = DT_DEV_PIPE_UNCHANGED;
  dt_pthread_mutex_unlock(&dev->history_mutex);
  dt_dev_pixelpipe_get_dimensions(pipe, dev, pipe->iwidth, pipe->iheight, &pipe->processed_width,
                                  &pipe->processed_height);
}

// TODO:
void dt_dev_pixelpipe_add_node(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int n)
{
}
// TODO:
void dt_dev_pixelpipe_remove_node(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int n)
{
}

static void get_output_format(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                              dt_develop_t *dev, dt_iop_buffer_dsc_t *dsc)
{
  if(module) return module->output_format(module, pipe, piece, dsc);

  // first input.
  *dsc = pipe->image.buf_dsc;

  if(!(pipe->image.flags & DT_IMAGE_RAW))
  {
    // image max is normalized before
    for(int k = 0; k < 4; k++) dsc->processed_maximum[k] = 1.0f;
  }
}


// helper to get per module histogram
static void histogram_collect(dt_dev_pixelpipe_iop_t *piece, const void *pixel, const dt_iop_roi_t *roi,
                              uint32_t **histogram, uint32_t *histogram_max)
{
  dt_dev_histogram_collection_params_t histogram_params = piece->histogram_params;

  dt_histogram_roi_t histogram_roi;

  // if the current module does did not specified its own ROI, use the full ROI
  if(histogram_params.roi == NULL)
  {
    histogram_roi = (dt_histogram_roi_t){
      .width = roi->width, .height = roi->height, .crop_x = 0, .crop_y = 0, .crop_width = 0, .crop_height = 0
    };

    histogram_params.roi = &histogram_roi;
  }

  const dt_iop_colorspace_type_t cst = dt_iop_module_colorspace(piece->module);

  dt_histogram_helper(&histogram_params, &piece->histogram_stats, cst, pixel, histogram);
  dt_histogram_max_helper(&piece->histogram_stats, cst, histogram, histogram_max);
}

#ifdef HAVE_OPENCL
// helper to get per module histogram for OpenCL
//
// this algorithm is inefficient as hell when it comes to larger images. it's only acceptable
// as long as we work on small image sizes like in image preview
static void histogram_collect_cl(int devid, dt_dev_pixelpipe_iop_t *piece, cl_mem img,
                                 const dt_iop_roi_t *roi, uint32_t **histogram, uint32_t *histogram_max,
                                 float *buffer, size_t bufsize)
{
  float *tmpbuf = NULL;
  float *pixel;

  // if buffer is supplied and if size fits let's use it
  if(buffer && bufsize >= (size_t)roi->width * roi->height * 4 * sizeof(float))
    pixel = buffer;
  else
    pixel = tmpbuf = dt_alloc_align(64, (size_t)roi->width * roi->height * 4 * sizeof(float));

  if(!pixel) return;

  cl_int err = dt_opencl_copy_device_to_host(devid, pixel, img, roi->width, roi->height, 4 * sizeof(float));
  if(err != CL_SUCCESS)
  {
    if(tmpbuf) dt_free_align(tmpbuf);
    return;
  }

  dt_dev_histogram_collection_params_t histogram_params = piece->histogram_params;

  dt_histogram_roi_t histogram_roi;

  // if the current module does did not specified its own ROI, use the full ROI
  if(histogram_params.roi == NULL)
  {
    histogram_roi = (dt_histogram_roi_t){
      .width = roi->width, .height = roi->height, .crop_x = 0, .crop_y = 0, .crop_width = 0, .crop_height = 0
    };

    histogram_params.roi = &histogram_roi;
  }

  const dt_iop_colorspace_type_t cst = dt_iop_module_colorspace(piece->module);

  dt_histogram_helper(&histogram_params, &piece->histogram_stats, cst, pixel, histogram);
  dt_histogram_max_helper(&piece->histogram_stats, cst, histogram, histogram_max);

  if(tmpbuf) dt_free_align(tmpbuf);
}
#endif

// helper for color picking
static int pixelpipe_picker_helper(dt_iop_module_t *module, const dt_iop_roi_t *roi, float *picked_color,
                                   float *picked_color_min, float *picked_color_max,
                                   dt_pixelpipe_picker_source_t picker_source, int *box)
{
  const float wd = darktable.develop->preview_pipe->backbuf_width;
  const float ht = darktable.develop->preview_pipe->backbuf_height;
  const int width = roi->width;
  const int height = roi->height;

  // initialize picker values. a positive value of picked_color_max[0] can later be used to check for validity
  // of data
  for(int k = 0; k < 4; k++) picked_color_min[k] = INFINITY;
  for(int k = 0; k < 4; k++) picked_color_max[k] = -INFINITY;
  for(int k = 0; k < 4; k++) picked_color[k] = 0.0f;

  // do not continue if one of the point coordinates is set to a negative value indicating a not yet defined
  // position
  if(module->color_picker_point[0] < 0 || module->color_picker_point[1] < 0) return 1;

  float fbox[4];

  // get absolute pixel coordinates in final preview image
  if(darktable.lib->proxy.colorpicker.size)
  {
    for(int k = 0; k < 4; k += 2) fbox[k] = module->color_picker_box[k] * wd;
    for(int k = 1; k < 4; k += 2) fbox[k] = module->color_picker_box[k] * ht;
  }
  else
  {
    fbox[0] = fbox[2] = module->color_picker_point[0] * wd;
    fbox[1] = fbox[3] = module->color_picker_point[1] * ht;
  }

  // transform back to current module coordinates
  dt_dev_distort_backtransform_plus(darktable.develop, darktable.develop->preview_pipe,
                                    module->priority + (picker_source == PIXELPIPE_PICKER_INPUT ? 0 : 1), 99999,
                                    fbox, 2);

  fbox[0] -= roi->x;
  fbox[1] -= roi->y;
  fbox[2] -= roi->x;
  fbox[3] -= roi->y;

  // re-order edges of bounding box
  box[0] = fminf(fbox[0], fbox[2]);
  box[1] = fminf(fbox[1], fbox[3]);
  box[2] = fmaxf(fbox[0], fbox[2]);
  box[3] = fmaxf(fbox[1], fbox[3]);

  if(!darktable.lib->proxy.colorpicker.size)
  {
    // if we are sampling one point, make sure that we actually sample it.
    for(int k = 2; k < 4; k++) box[k] += 1;
  }

  // do not continue if box is completely outside of roi
  if(box[0] >= width || box[1] >= height || box[2] < 0 || box[3] < 0) return 1;

  // clamp bounding box to roi
  for(int k = 0; k < 4; k += 2) box[k] = MIN(width - 1, MAX(0, box[k]));
  for(int k = 1; k < 4; k += 2) box[k] = MIN(height - 1, MAX(0, box[k]));

  // safety check: area needs to have minimum 1 pixel width and height
  if(box[2] - box[0] < 1 || box[3] - box[1] < 1) return 1;

  return 0;
}

static void pixelpipe_picker(dt_iop_module_t *module, dt_iop_buffer_dsc_t *dsc, const float *pixel,
                             const dt_iop_roi_t *roi, float *picked_color, float *picked_color_min,
                             float *picked_color_max, dt_pixelpipe_picker_source_t picker_source)
{
  int box[4];

  if(pixelpipe_picker_helper(module, roi, picked_color, picked_color_min, picked_color_max, picker_source, box))
    return;

  dt_color_picker_helper(dsc, pixel, roi, box, picked_color, picked_color_min, picked_color_max);
}


#ifdef HAVE_OPENCL
// helper for OpenCL color picking
//
// this algorithm is inefficient as hell when it comes to larger images. it's only acceptable
// as long as we work on small image sizes like in image preview
static void pixelpipe_picker_cl(int devid, dt_iop_module_t *module, dt_iop_buffer_dsc_t *dsc, cl_mem img,
                                const dt_iop_roi_t *roi, float *picked_color, float *picked_color_min,
                                float *picked_color_max, float *buffer, size_t bufsize,
                                dt_pixelpipe_picker_source_t picker_source)
{
  int box[4];

  if(pixelpipe_picker_helper(module, roi, picked_color, picked_color_min, picked_color_max, picker_source, box))
    return;

  size_t origin[3];
  size_t region[3];

  // Initializing bounds of colorpicker box
  origin[0] = box[0];
  origin[1] = box[1];
  origin[2] = 0;

  region[0] = box[2] - box[0];
  region[1] = box[3] - box[1];
  region[2] = 1;

  float *pixel;
  float *tmpbuf = NULL;

  const size_t size = region[0] * region[1];

  const size_t bpp = dt_iop_buffer_dsc_to_bpp(dsc);

  // if a buffer is supplied and if size fits let's use it
  if(buffer && bufsize >= size * bpp)
    pixel = buffer;
  else
    pixel = tmpbuf = dt_alloc_align(64, size * bpp);

  if(pixel == NULL) return;

  // get the required part of the image from opencl device
  cl_int err = dt_opencl_read_host_from_device_raw(devid, pixel, img, origin, region, region[0] * bpp, CL_TRUE);

  if(err != CL_SUCCESS) goto error;

  dt_iop_roi_t roi_copy = (dt_iop_roi_t){.x = roi->x + box[0], .y = roi->y + box[1], .width = region[0], .height = region[1] };

  box[0] = 0;
  box[1] = 0;
  box[2] = region[0];
  box[3] = region[1];

  dt_color_picker_helper(dsc, pixel, &roi_copy, box, picked_color, picked_color_min, picked_color_max);

error:
  dt_free_align(tmpbuf);
}
#endif


// recursive helper for process:
static int dt_dev_pixelpipe_process_rec(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, void **output,
                                        void **cl_mem_output, dt_iop_buffer_dsc_t **out_format,
                                        const dt_iop_roi_t *roi_out, GList *modules, GList *pieces, int pos)
{
  dt_iop_roi_t roi_in = *roi_out;

  char module_name[256] = { 0 };
  void *input = NULL;
  void *cl_mem_input = NULL;
  *cl_mem_output = NULL;
  dt_iop_module_t *module = NULL;
  dt_dev_pixelpipe_iop_t *piece = NULL;
  if(modules)
  {
    module = (dt_iop_module_t *)modules->data;
    piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
    // skip this module?
    if(!piece->enabled
       || (dev->gui_module && dev->gui_module->operation_tags_filter() & module->operation_tags()))
      return dt_dev_pixelpipe_process_rec(pipe, dev, output, cl_mem_output, out_format, &roi_in,
                                          g_list_previous(modules), g_list_previous(pieces), pos - 1);
  }

  if(module) g_strlcpy(module_name, module->op, MIN(sizeof(module_name), sizeof(module->op)));
  get_output_format(module, pipe, piece, dev, *out_format);
  const size_t bpp = dt_iop_buffer_dsc_to_bpp(*out_format);
  const size_t bufsize = (size_t)bpp * roi_out->width * roi_out->height;

  // 1) if cached buffer is still available, return data
  dt_pthread_mutex_lock(&pipe->busy_mutex);
  if(pipe->shutdown)
  {
    dt_pthread_mutex_unlock(&pipe->busy_mutex);
    return 1;
  }
  uint64_t hash = dt_dev_pixelpipe_cache_hash(pipe->image.id, roi_out, pipe, pos);
  if(dt_dev_pixelpipe_cache_available(&(pipe->cache), hash))
  {
    // if(module) printf("found valid buf pos %d in cache for module %s %s %lu\n", pos, module->op, pipe ==
    // dev->preview_pipe ? "[preview]" : "", hash);

    (void)dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, bufsize, output, out_format);

    dt_pthread_mutex_unlock(&pipe->busy_mutex);
    if(!modules) return 0;
    // go to post-collect directly:
    goto post_process_collect_info;
  }
  else
    dt_pthread_mutex_unlock(&pipe->busy_mutex);

  // 2) if history changed or exit event, abort processing?
  // preview pipe: abort on all but zoom events (same buffer anyways)
  if(dt_iop_breakpoint(dev, pipe)) return 1;
  // if image has changed, stop now.
  if(pipe == dev->pipe && dev->image_force_reload) return 1;
  if(pipe == dev->preview_pipe && dev->preview_loading) return 1;
  if(dev->gui_leaving) return 1;


  // 3) input -> output
  if(!modules)
  {
    // 3a) import input array with given scale and roi
    dt_pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown)
    {
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      return 1;
    }
    dt_times_t start;
    dt_get_times(&start);
    // we're looking for the full buffer
    {
      if(roi_out->scale == 1.0 && roi_out->x == 0 && roi_out->y == 0 && pipe->iwidth == roi_out->width
         && pipe->iheight == roi_out->height)
      {
        *output = pipe->input;
      }
      else if(dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, bufsize, output, out_format))
      {
        memset(*output, 0, bufsize);
        if(roi_in.scale == 1.0f)
        {
          // fast branch for 1:1 pixel copies.

          // last minute clamping to catch potential out-of-bounds in roi_in and roi_out

          const int in_x = MAX(roi_in.x, 0);
          const int in_y = MAX(roi_in.y, 0);
          const int cp_width = MIN(roi_out->width, pipe->iwidth - in_x);
          const int cp_height = MIN(roi_out->height, pipe->iheight - in_y);

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(pipe, roi_out, roi_in, output)
#endif
          for(int j = 0; j < cp_height; j++)
            memcpy(((char *)*output) + (size_t)bpp * j * roi_out->width,
                   ((char *)pipe->input) + (size_t)bpp * (in_x + (in_y + j) * pipe->iwidth),
                   (size_t)bpp * cp_width);
        }
        else
        {
          roi_in.x /= roi_out->scale;
          roi_in.y /= roi_out->scale;
          roi_in.width = pipe->iwidth;
          roi_in.height = pipe->iheight;
          roi_in.scale = 1.0f;
          dt_iop_clip_and_zoom(*output, pipe->input, roi_out, &roi_in, roi_out->width, pipe->iwidth);
        }
      }
      // else found in cache.
    }

    dt_show_times(&start, "[dev_pixelpipe]", "initing base buffer [%s]", _pipe_type_to_str(pipe->type));
    dt_pthread_mutex_unlock(&pipe->busy_mutex);
  }
  else
  {
    // 3b) recurse and obtain output array in &input

    // get region of interest which is needed in input
    dt_pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown)
    {
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      return 1;
    }
    module->modify_roi_in(module, piece, roi_out, &roi_in);
    dt_pthread_mutex_unlock(&pipe->busy_mutex);

    // recurse to get actual data of input buffer

    dt_iop_buffer_dsc_t _input_format = { 0 };
    dt_iop_buffer_dsc_t *input_format = &_input_format;

    piece = (dt_dev_pixelpipe_iop_t *)pieces->data;

    if(dt_dev_pixelpipe_process_rec(pipe, dev, &input, &cl_mem_input, &input_format, &roi_in,
                                    g_list_previous(modules), g_list_previous(pieces), pos - 1))
      return 1;

    const size_t in_bpp = dt_iop_buffer_dsc_to_bpp(input_format);

    piece->dsc_out = piece->dsc_in = *input_format;

    module->output_format(module, pipe, piece, &piece->dsc_out);

    **out_format = pipe->dsc = piece->dsc_out;

    const size_t out_bpp = dt_iop_buffer_dsc_to_bpp(*out_format);

    // reserve new cache line: output
    dt_pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown)
    {
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      return 1;
    }

    if(!strcmp(module->op, "gamma"))
      (void)dt_dev_pixelpipe_cache_get_important(&(pipe->cache), hash, bufsize, output, out_format);
    else
      (void)dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, bufsize, output, out_format);

    dt_pthread_mutex_unlock(&pipe->busy_mutex);

// if(module) printf("reserving new buf in cache for module %s %s: %ld buf %p\n", module->op, pipe ==
// dev->preview_pipe ? "[preview]" : "", hash, *output);

    dt_pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown)
    {
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      return 1;
    }

    dt_times_t start;
    dt_get_times(&start);

    dt_pixelpipe_flow_t pixelpipe_flow = (PIXELPIPE_FLOW_NONE | PIXELPIPE_FLOW_HISTOGRAM_NONE);

    // special case: user requests to see channel data in the parametric mask of a module. In that case
    // we skip all modules manipulating pixel content and only process image distorting modules. Finally
    // "gamma" is responsible to display channel data accordingly.
    if(strcmp(module->op, "gamma") && (pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY) && !(module->operation_tags() & IOP_TAG_DISTORT) &&
      (in_bpp == out_bpp) && !memcmp(&roi_in, roi_out, sizeof(struct dt_iop_roi_t)))
    {
#ifdef HAVE_OPENCL
      if(dt_opencl_is_inited() && pipe->opencl_enabled && pipe->devid >= 0 && (cl_mem_input != NULL))
      {
        *cl_mem_output = cl_mem_input;
      }
      else
      {
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(roi_out, roi_in, output, input)
#endif
        for(int j = 0; j < roi_out->height; j++)
            memcpy(((char *)*output) + (size_t)out_bpp * j * roi_out->width,
                   ((char *)input) + (size_t)in_bpp * j * roi_in.width,
                   (size_t)in_bpp * roi_in.width);
      }
#else // don't HAVE_OPENCL
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(roi_out, roi_in, output, input)
#endif
      for(int j = 0; j < roi_out->height; j++)
            memcpy(((char *)*output) + (size_t)out_bpp * j * roi_out->width,
                   ((char *)input) + (size_t)in_bpp * j * roi_in.width,
                   (size_t)in_bpp * roi_in.width);
#endif

      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      return 0;
    }


    /* get tiling requirement of module */
    dt_develop_tiling_t tiling = { 0 };
    module->tiling_callback(module, piece, &roi_in, roi_out, &tiling);

    /* does this module involve blending? */
    if(piece->blendop_data && (dt_develop_blend_params_t *)piece->blendop_data != DEVELOP_MASK_DISABLED)
    {
      /* get specific memory requirement for blending */
      dt_develop_tiling_t tiling_blendop = { 0 };
      tiling_callback_blendop(module, piece, &roi_in, roi_out, &tiling_blendop);

      /* aggregate in structure tiling */
      tiling.factor = fmax(tiling.factor, tiling_blendop.factor);
      tiling.maxbuf = fmax(tiling.maxbuf, tiling_blendop.maxbuf);
      tiling.overhead = fmax(tiling.overhead, tiling_blendop.overhead);
    }

    /* remark: we do not do tiling for blendop step, neither in opencl nor on cpu. if overall tiling
       requirements (maximum of module and blendop) require tiling for opencl path, then following blend
       step is anyhow done on cpu. we assume that blending itself will never require tiling in cpu path,
       because memory requirements will still be low enough. */

    assert(tiling.factor > 0.0f);

    if(pipe->shutdown)
    {
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      return 1;
    }

#ifdef HAVE_OPENCL
    /* do we have opencl at all? did user tell us to use it? did we get a resource? */
    if(dt_opencl_is_inited() && pipe->opencl_enabled && pipe->devid >= 0)
    {
      int success_opencl = TRUE;

      /* if input is on gpu memory only, remember this fact to later take appropriate action */
      int valid_input_on_gpu_only = (cl_mem_input != NULL);

      /* pre-check if there is enough space on device for non-tiled processing */
      const int fits_on_device = dt_opencl_image_fits_device(pipe->devid, MAX(roi_in.width, roi_out->width),
                                                             MAX(roi_in.height, roi_out->height), MAX(in_bpp, bpp),
                                                             tiling.factor, tiling.overhead);

      /* general remark: in case of opencl errors within modules or out-of-memory on GPU, we transparently
         fall back to the respective cpu module and continue in pixelpipe. If we encounter errors we set
         pipe->opencl_error=1, return this function with value 1, and leave appropriate action to the calling
         function, which normally would restart pixelpipe without opencl.
         Late errors are sometimes detected when trying to get back data from device into host memory and
         are treated in the same manner. */

      /* try to enter opencl path after checking some module specific pre-requisites */
      if(module->process_cl && piece->process_cl_ready
         && !((pipe->type == DT_DEV_PIXELPIPE_PREVIEW) && (module->flags() & IOP_FLAGS_PREVIEW_NON_OPENCL))
         && (fits_on_device || piece->process_tiling_ready))
      {

        // fprintf(stderr, "[opencl_pixelpipe 0] factor %f, overhead %d, width %d, height %d, bpp %d\n",
        // (double)tiling.factor, tiling.overhead, roi_in.width, roi_in.height, bpp);

        // fprintf(stderr, "[opencl_pixelpipe 1] for module `%s', have bufs %p and %p \n", module->op,
        // cl_mem_input, *cl_mem_output);
        // fprintf(stderr, "[opencl_pixelpipe 1] module '%s'\n", module->op);

        if(fits_on_device)
        {
          /* image is small enough -> try to directly process entire image with opencl */

          // fprintf(stderr, "[opencl_pixelpipe 2] module '%s' running directly with process_cl\n",
          // module->op);

          /* input is not on gpu memory -> copy it there */
          if(cl_mem_input == NULL)
          {
            cl_mem_input = dt_opencl_alloc_device(pipe->devid, roi_in.width, roi_in.height, in_bpp);
            if(cl_mem_input == NULL)
            {
              dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] couldn't generate input buffer for module %s\n",
                       module->op);
              success_opencl = FALSE;
            }

            if(success_opencl)
            {
              cl_int err = dt_opencl_write_host_to_device(pipe->devid, input, cl_mem_input,
                                                                       roi_in.width, roi_in.height, in_bpp);
              if(err != CL_SUCCESS)
              {
                dt_print(DT_DEBUG_OPENCL,
                         "[opencl_pixelpipe] couldn't copy image to opencl device for module %s\n",
                         module->op);
                success_opencl = FALSE;
              }
            }
          }

          if(pipe->shutdown)
          {
            dt_opencl_release_mem_object(cl_mem_input);
            dt_pthread_mutex_unlock(&pipe->busy_mutex);
            return 1;
          }

          /* try to allocate GPU memory for output */
          if(success_opencl)
          {
            *cl_mem_output = dt_opencl_alloc_device(pipe->devid, roi_out->width, roi_out->height, bpp);
            if(*cl_mem_output == NULL)
            {
              dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] couldn't allocate output buffer for module %s\n",
                       module->op);
              success_opencl = FALSE;
            }
          }

          // fprintf(stderr, "[opencl_pixelpipe 2] for module `%s', have bufs %p and %p \n", module->op,
          // cl_mem_input, *cl_mem_output);

          // indirectly give gpu some air to breathe (and to do display related stuff)
          dt_iop_nap(darktable.opencl->micro_nap);

          // histogram collection for module
          if(success_opencl && (dev->gui_attached || !(piece->request_histogram & DT_REQUEST_ONLY_IN_GUI))
             && (piece->request_histogram & DT_REQUEST_ON))
          {
            // we abuse the empty output buffer on host for intermediate storage of data in
            // histogram_collect_cl()
            size_t outbufsize = roi_out->width * roi_out->height * bpp;

            histogram_collect_cl(pipe->devid, piece, cl_mem_input, &roi_in, &(piece->histogram),
                                 piece->histogram_max, *output, outbufsize);
            pixelpipe_flow |= (PIXELPIPE_FLOW_HISTOGRAM_ON_GPU);
            pixelpipe_flow &= ~(PIXELPIPE_FLOW_HISTOGRAM_NONE | PIXELPIPE_FLOW_HISTOGRAM_ON_CPU);

            if(piece->histogram && (module->request_histogram & DT_REQUEST_ON)
               && pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
            {
              const size_t buf_size = 4 * piece->histogram_stats.bins_count * sizeof(uint32_t);
              module->histogram = realloc(module->histogram, buf_size);
              memcpy(module->histogram, piece->histogram, buf_size);
              module->histogram_stats = piece->histogram_stats;
              memcpy(module->histogram_max, piece->histogram_max, sizeof(piece->histogram_max));

              dt_pthread_mutex_unlock(&pipe->busy_mutex);

              if(module->widget) dt_control_queue_redraw_widget(module->widget);

              dt_pthread_mutex_lock(&pipe->busy_mutex);
            }
          }

          if(pipe->shutdown)
          {
            dt_pthread_mutex_unlock(&pipe->busy_mutex);
            return 1;
          }

          /* now call process_cl of module; module should emit meaningful messages in case of error */
          if(success_opencl)
          {
            success_opencl
                = module->process_cl(module, piece, cl_mem_input, *cl_mem_output, &roi_in, roi_out);
            pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_GPU);
            pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_CPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
          }

          if(pipe->shutdown)
          {
            dt_opencl_release_mem_object(cl_mem_input);
            dt_pthread_mutex_unlock(&pipe->busy_mutex);
            return 1;
          }

          // Lab color picking for module
          if(success_opencl && dev->gui_attached && pipe == dev->preview_pipe
             &&                           // pick from preview pipe to get pixels outside the viewport
             module == dev->gui_module && // only modules with focus can pick
             module->request_color_pick != DT_REQUEST_COLORPICK_OFF) // and they want to pick ;)
          {

            // we abuse the empty output buffer on host for intermediate storage of data in
            // pixelpipe_picker_cl()
            size_t outbufsize = roi_out->width * roi_out->height * bpp;

            pixelpipe_picker_cl(pipe->devid, module, &piece->dsc_in, cl_mem_input, &roi_in, module->picked_color,
                                module->picked_color_min, module->picked_color_max, *output, outbufsize,
                                PIXELPIPE_PICKER_INPUT);
            pixelpipe_picker_cl(pipe->devid, module, &pipe->dsc, (*cl_mem_output), roi_out,
                                module->picked_output_color, module->picked_output_color_min,
                                module->picked_output_color_max, *output, outbufsize, PIXELPIPE_PICKER_OUTPUT);

            dt_pthread_mutex_unlock(&pipe->busy_mutex);

            if(module->widget) dt_control_queue_redraw_widget(module->widget);

            dt_pthread_mutex_lock(&pipe->busy_mutex);
          }

          if(pipe->shutdown)
          {
            dt_pthread_mutex_unlock(&pipe->busy_mutex);
            return 1;
          }

          /* process blending */
          if(success_opencl)
          {
            success_opencl
                = dt_develop_blend_process_cl(module, piece, cl_mem_input, *cl_mem_output, &roi_in, roi_out);
            pixelpipe_flow |= (PIXELPIPE_FLOW_BLENDED_ON_GPU);
            pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_CPU);
          }

          /* synchronization point for opencl pipe */
          if(success_opencl && (!darktable.opencl->async_pixelpipe || pipe->type == DT_DEV_PIXELPIPE_EXPORT))
            success_opencl = dt_opencl_finish(pipe->devid);


          if(pipe->shutdown)
          {
            dt_opencl_release_mem_object(cl_mem_input);
            dt_pthread_mutex_unlock(&pipe->busy_mutex);
            return 1;
          }
        }
        else if(piece->process_tiling_ready)
        {
          /* image is too big for direct opencl processing -> try to process image via tiling */

          // fprintf(stderr, "[opencl_pixelpipe 3] module '%s' tiling with process_tiling_cl\n", module->op);

          /* we might need to copy back valid image from device to host */
          if(cl_mem_input != NULL)
          {
            cl_int err;

            /* copy back to CPU buffer, then clean unneeded buffer */
            err = dt_opencl_copy_device_to_host(pipe->devid, input, cl_mem_input, roi_in.width, roi_in.height,
                                                in_bpp);
            if(err != CL_SUCCESS)
            {
              /* late opencl error */
              dt_print(
                  DT_DEBUG_OPENCL,
                  "[opencl_pixelpipe (a)] late opencl error detected while copying back to cpu buffer: %d\n",
                  err);
              dt_opencl_release_mem_object(cl_mem_input);
              pipe->opencl_error = 1;
              dt_pthread_mutex_unlock(&pipe->busy_mutex);
              return 1;
            }
            dt_opencl_release_mem_object(cl_mem_input);
            cl_mem_input = NULL;
            valid_input_on_gpu_only = FALSE;
          }

          if(pipe->shutdown)
          {
            dt_pthread_mutex_unlock(&pipe->busy_mutex);
            return 1;
          }

          // indirectly give gpu some air to breathe (and to do display related stuff)
          dt_iop_nap(darktable.opencl->micro_nap);

          // histogram collection for module
          if(success_opencl && (dev->gui_attached || !(piece->request_histogram & DT_REQUEST_ONLY_IN_GUI))
             && (piece->request_histogram & DT_REQUEST_ON))
          {
            histogram_collect(piece, input, &roi_in, &(piece->histogram), piece->histogram_max);
            pixelpipe_flow |= (PIXELPIPE_FLOW_HISTOGRAM_ON_CPU);
            pixelpipe_flow &= ~(PIXELPIPE_FLOW_HISTOGRAM_NONE | PIXELPIPE_FLOW_HISTOGRAM_ON_GPU);

            if(piece->histogram && (module->request_histogram & DT_REQUEST_ON)
               && pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
            {
              const size_t buf_size = 4 * piece->histogram_stats.bins_count * sizeof(uint32_t);
              module->histogram = realloc(module->histogram, buf_size);
              memcpy(module->histogram, piece->histogram, buf_size);
              module->histogram_stats = piece->histogram_stats;
              memcpy(module->histogram_max, piece->histogram_max, sizeof(piece->histogram_max));

              dt_pthread_mutex_unlock(&pipe->busy_mutex);

              if(module->widget) dt_control_queue_redraw_widget(module->widget);

              dt_pthread_mutex_lock(&pipe->busy_mutex);
            }
          }

          if(pipe->shutdown)
          {
            dt_pthread_mutex_unlock(&pipe->busy_mutex);
            return 1;
          }

          /* now call process_tiling_cl of module; module should emit meaningful messages in case of error */
          if(success_opencl)
          {
            success_opencl
                = module->process_tiling_cl(module, piece, input, *output, &roi_in, roi_out, in_bpp);
            pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_GPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
            pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_CPU);
          }

          if(pipe->shutdown)
          {
            dt_pthread_mutex_unlock(&pipe->busy_mutex);
            return 1;
          }

          // Lab color picking for module
          if(success_opencl && dev->gui_attached && pipe == dev->preview_pipe
             &&                           // pick from preview pipe to get pixels outside the viewport
             module == dev->gui_module && // only modules with focus can pick
             module->request_color_pick != DT_REQUEST_COLORPICK_OFF) // and they want to pick ;)
          {
            pixelpipe_picker(module, &piece->dsc_in, (float *)input, &roi_in, module->picked_color,
                             module->picked_color_min, module->picked_color_max, PIXELPIPE_PICKER_INPUT);
            pixelpipe_picker(module, &pipe->dsc, (float *)(*output), roi_out, module->picked_output_color,
                             module->picked_output_color_min, module->picked_output_color_max,
                             PIXELPIPE_PICKER_OUTPUT);

            dt_pthread_mutex_unlock(&pipe->busy_mutex);

            if(module->widget) dt_control_queue_redraw_widget(module->widget);

            dt_pthread_mutex_lock(&pipe->busy_mutex);
          }

          if(pipe->shutdown)
          {
            dt_pthread_mutex_unlock(&pipe->busy_mutex);
            return 1;
          }

          /* do process blending on cpu (this is anyhow fast enough) */
          if(success_opencl)
          {
            dt_develop_blend_process(module, piece, input, *output, &roi_in, roi_out);
            pixelpipe_flow |= (PIXELPIPE_FLOW_BLENDED_ON_CPU);
            pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_GPU);
          }

          /* synchronization point for opencl pipe */
          if(success_opencl && (!darktable.opencl->async_pixelpipe || pipe->type == DT_DEV_PIXELPIPE_EXPORT))
            success_opencl = dt_opencl_finish(pipe->devid);

          if(pipe->shutdown)
          {
            dt_pthread_mutex_unlock(&pipe->busy_mutex);
            return 1;
          }
        }
        else
        {
          /* image is too big for direct opencl and tiling is not allowed -> no opencl processing for this
           * module */
          success_opencl = FALSE;
        }

        if(pipe->shutdown)
        {
          dt_opencl_release_mem_object(cl_mem_input);
          dt_pthread_mutex_unlock(&pipe->busy_mutex);
          return 1;
        }

        // if (rand() % 20 == 0) success_opencl = FALSE; // Test code: simulate spurious failures

        /* finally check, if we were successful */
        if(success_opencl)
        {
          /* Nice, everything went fine */

          /* this is reasonable on slow GPUs only, where it's more expensive to reprocess the whole pixelpipe
             than
             regularly copying device buffers back to host. This would slow down fast GPUs considerably. */
          if(darktable.opencl->synch_cache)
          {
            /* write back input into cache for faster re-usal (not for export or thumbnails) */
            if(cl_mem_input != NULL && pipe->type != DT_DEV_PIXELPIPE_EXPORT
               && pipe->type != DT_DEV_PIXELPIPE_THUMBNAIL)
            {
              cl_int err;

              /* copy input to host memory, so we can find it in cache */
              err = dt_opencl_copy_device_to_host(pipe->devid, input, cl_mem_input, roi_in.width,
                                                  roi_in.height, in_bpp);
              if(err != CL_SUCCESS)
              {
                /* late opencl error, not likely to happen here */
                dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe (e)] late opencl error detected while copying "
                                          "back to cpu buffer: %d\n",
                         err);
                /* that's all we do here, we later make sure to invalidate cache line */
              }
              else
              {
                /* success: cache line is valid now, so we will not need to invalidate it later */
                valid_input_on_gpu_only = FALSE;

                // TODO: check if we need to wait for finished opencl pipe before we release cl_mem_input
                // dt_dev_finish(pipe->devid);
              }
            }

            if(pipe->shutdown)
            {
              dt_opencl_release_mem_object(cl_mem_input);
              dt_pthread_mutex_unlock(&pipe->busy_mutex);
              return 1;
            }
          }

          /* we can now release cl_mem_input */
          dt_opencl_release_mem_object(cl_mem_input);
          cl_mem_input = NULL;
          // we speculate on the next plug-in to possibly copy back cl_mem_output to output,
          // so we're not just yet invalidating the (empty) output cache line.
        }
        else
        {
          /* Bad luck, opencl failed. Let's clean up and fall back to cpu module */
          dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] could not run module '%s' on gpu. falling back to cpu path\n",
                   module->op);

          // fprintf(stderr, "[opencl_pixelpipe 4] module '%s' running on cpu\n", module->op);

          /* we might need to free unused output buffer */
          if(*cl_mem_output != NULL)
          {
            dt_opencl_release_mem_object(*cl_mem_output);
            *cl_mem_output = NULL;
          }

          /* check where our input buffer is located */
          if(cl_mem_input != NULL)
          {
            cl_int err;

            /* copy back to host memory, then clean no longer needed opencl buffer.
               important info: in order to make this possible, opencl modules must
               not spoil their input buffer, even in case of errors. */
            err = dt_opencl_copy_device_to_host(pipe->devid, input, cl_mem_input, roi_in.width, roi_in.height,
                                                in_bpp);
            if(err != CL_SUCCESS)
            {
              /* late opencl error */
              dt_print(
                  DT_DEBUG_OPENCL,
                  "[opencl_pixelpipe (b)] late opencl error detected while copying back to cpu buffer: %d\n",
                  err);
              dt_opencl_release_mem_object(cl_mem_input);
              pipe->opencl_error = 1;
              dt_pthread_mutex_unlock(&pipe->busy_mutex);
              return 1;
            }

            /* this is a good place to release event handles as we anyhow need to move from gpu to cpu here */
            (void)dt_opencl_finish(pipe->devid);
            dt_opencl_release_mem_object(cl_mem_input);
            valid_input_on_gpu_only = FALSE;
          }

          if(pipe->shutdown)
          {
            dt_pthread_mutex_unlock(&pipe->busy_mutex);
            return 1;
          }

          // histogram collection for module
          if((dev->gui_attached || !(piece->request_histogram & DT_REQUEST_ONLY_IN_GUI))
             && (piece->request_histogram & DT_REQUEST_ON))
          {
            histogram_collect(piece, input, &roi_in, &(piece->histogram), piece->histogram_max);
            pixelpipe_flow |= (PIXELPIPE_FLOW_HISTOGRAM_ON_CPU);
            pixelpipe_flow &= ~(PIXELPIPE_FLOW_HISTOGRAM_NONE | PIXELPIPE_FLOW_HISTOGRAM_ON_GPU);

            if(piece->histogram && (module->request_histogram & DT_REQUEST_ON)
               && pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
            {
              const size_t buf_size = 4 * piece->histogram_stats.bins_count * sizeof(uint32_t);
              module->histogram = realloc(module->histogram, buf_size);
              memcpy(module->histogram, piece->histogram, buf_size);
              module->histogram_stats = piece->histogram_stats;
              memcpy(module->histogram_max, piece->histogram_max, sizeof(piece->histogram_max));

              dt_pthread_mutex_unlock(&pipe->busy_mutex);

              if(module->widget) dt_control_queue_redraw_widget(module->widget);

              dt_pthread_mutex_lock(&pipe->busy_mutex);
            }
          }

          if(pipe->shutdown)
          {
            dt_pthread_mutex_unlock(&pipe->busy_mutex);
            return 1;
          }

          /* process module on cpu. use tiling if needed and possible. */
          if(piece->process_tiling_ready
             && !dt_tiling_piece_fits_host_memory(MAX(roi_in.width, roi_out->width),
                                                  MAX(roi_in.height, roi_out->height), MAX(in_bpp, bpp),
                                                  tiling.factor, tiling.overhead))
          {
            module->process_tiling(module, piece, input, *output, &roi_in, roi_out, in_bpp);
            pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
            pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU);
          }
          else
          {
            module->process(module, piece, input, *output, &roi_in, roi_out);
            pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU);
            pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
          }

          if(pipe->shutdown)
          {
            dt_pthread_mutex_unlock(&pipe->busy_mutex);
            return 1;
          }

          // Lab color picking for module
          if(dev->gui_attached && pipe == dev->preview_pipe
             &&                           // pick from preview pipe to get pixels outside the viewport
             module == dev->gui_module && // only modules with focus can pick
             module->request_color_pick != DT_REQUEST_COLORPICK_OFF) // and they want to pick ;)
          {
            pixelpipe_picker(module, &piece->dsc_in, (float *)input, &roi_in, module->picked_color,
                             module->picked_color_min, module->picked_color_max, PIXELPIPE_PICKER_INPUT);
            pixelpipe_picker(module, &pipe->dsc, (float *)(*output), roi_out, module->picked_output_color,
                             module->picked_output_color_min, module->picked_output_color_max,
                             PIXELPIPE_PICKER_OUTPUT);

            dt_pthread_mutex_unlock(&pipe->busy_mutex);

            if(module->widget) dt_control_queue_redraw_widget(module->widget);

            dt_pthread_mutex_lock(&pipe->busy_mutex);
          }

          if(pipe->shutdown)
          {
            dt_pthread_mutex_unlock(&pipe->busy_mutex);
            return 1;
          }

          /* process blending on cpu */
          dt_develop_blend_process(module, piece, input, *output, &roi_in, roi_out);
          pixelpipe_flow |= (PIXELPIPE_FLOW_BLENDED_ON_CPU);
          pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_GPU);
        }

        if(pipe->shutdown)
        {
          dt_pthread_mutex_unlock(&pipe->busy_mutex);
          return 1;
        }
      }
      else
      {
        /* we are not allowed to use opencl for this module */

        // fprintf(stderr, "[opencl_pixelpipe 3] for module `%s', have bufs %p and %p \n", module->op,
        // cl_mem_input, *cl_mem_output);

        *cl_mem_output = NULL;

        /* cleanup unneeded opencl buffer, and copy back to CPU buffer */
        if(cl_mem_input != NULL)
        {
          cl_int err;

          err = dt_opencl_copy_device_to_host(pipe->devid, input, cl_mem_input, roi_in.width, roi_in.height,
                                              in_bpp);
          // if (rand() % 5 == 0) err = !CL_SUCCESS; // Test code: simulate spurious failures
          if(err != CL_SUCCESS)
          {
            /* late opencl error */
            dt_print(
                DT_DEBUG_OPENCL,
                "[opencl_pixelpipe (c)] late opencl error detected while copying back to cpu buffer: %d\n",
                err);
            dt_opencl_release_mem_object(cl_mem_input);
            pipe->opencl_error = 1;
            dt_pthread_mutex_unlock(&pipe->busy_mutex);
            return 1;
          }

          /* this is a good place to release event handles as we anyhow need to move from gpu to cpu here */
          (void)dt_opencl_finish(pipe->devid);
          dt_opencl_release_mem_object(cl_mem_input);
          valid_input_on_gpu_only = FALSE;
        }

        if(pipe->shutdown)
        {
          dt_pthread_mutex_unlock(&pipe->busy_mutex);
          return 1;
        }

        // histogram collection for module
        if((dev->gui_attached || !(piece->request_histogram & DT_REQUEST_ONLY_IN_GUI))
           && (piece->request_histogram & DT_REQUEST_ON))
        {
          histogram_collect(piece, input, &roi_in, &(piece->histogram), piece->histogram_max);
          pixelpipe_flow |= (PIXELPIPE_FLOW_HISTOGRAM_ON_CPU);
          pixelpipe_flow &= ~(PIXELPIPE_FLOW_HISTOGRAM_NONE | PIXELPIPE_FLOW_HISTOGRAM_ON_GPU);

          if(piece->histogram && (module->request_histogram & DT_REQUEST_ON)
             && pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
          {
            const size_t buf_size = 4 * piece->histogram_stats.bins_count * sizeof(uint32_t);
            module->histogram = realloc(module->histogram, buf_size);
            memcpy(module->histogram, piece->histogram, buf_size);
            module->histogram_stats = piece->histogram_stats;
            memcpy(module->histogram_max, piece->histogram_max, sizeof(piece->histogram_max));

            dt_pthread_mutex_unlock(&pipe->busy_mutex);

            if(module->widget) dt_control_queue_redraw_widget(module->widget);

            dt_pthread_mutex_lock(&pipe->busy_mutex);
          }
        }

        if(pipe->shutdown)
        {
          dt_pthread_mutex_unlock(&pipe->busy_mutex);
          return 1;
        }

        /* process module on cpu. use tiling if needed and possible. */
        if(piece->process_tiling_ready
           && !dt_tiling_piece_fits_host_memory(MAX(roi_in.width, roi_out->width),
                                                MAX(roi_in.height, roi_out->height), MAX(in_bpp, bpp),
                                                tiling.factor, tiling.overhead))
        {
          module->process_tiling(module, piece, input, *output, &roi_in, roi_out, in_bpp);
          pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
          pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU);
        }
        else
        {
          module->process(module, piece, input, *output, &roi_in, roi_out);
          pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU);
          pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
        }

        if(pipe->shutdown)
        {
          dt_pthread_mutex_unlock(&pipe->busy_mutex);
          return 1;
        }

        // Lab color picking for module
        if(dev->gui_attached && pipe == dev->preview_pipe
           &&                           // pick from preview pipe to get pixels outside the viewport
           module == dev->gui_module && // only modules with focus can pick
           module->request_color_pick != DT_REQUEST_COLORPICK_OFF) // and they want to pick ;)
        {
          pixelpipe_picker(module, &piece->dsc_in, (float *)input, &roi_in, module->picked_color,
                           module->picked_color_min, module->picked_color_max, PIXELPIPE_PICKER_INPUT);
          pixelpipe_picker(module, &pipe->dsc, (float *)(*output), roi_out, module->picked_output_color,
                           module->picked_output_color_min, module->picked_output_color_max,
                           PIXELPIPE_PICKER_OUTPUT);

          dt_pthread_mutex_unlock(&pipe->busy_mutex);

          if(module->widget) dt_control_queue_redraw_widget(module->widget);

          dt_pthread_mutex_lock(&pipe->busy_mutex);
        }

        if(pipe->shutdown)
        {
          dt_pthread_mutex_unlock(&pipe->busy_mutex);
          return 1;
        }

        /* process blending */
        dt_develop_blend_process(module, piece, input, *output, &roi_in, roi_out);
        pixelpipe_flow |= (PIXELPIPE_FLOW_BLENDED_ON_CPU);
        pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_GPU);

        if(pipe->shutdown)
        {
          dt_pthread_mutex_unlock(&pipe->busy_mutex);
          return 1;
        }
      }

      /* input is still only on GPU? Let's invalidate CPU input buffer then */
      if(valid_input_on_gpu_only) dt_dev_pixelpipe_cache_invalidate(&(pipe->cache), input);
    }
    else
    {
      /* opencl is not inited or not enabled or we got no resource/device -> everything runs on cpu */

      // histogram collection for module
      if((dev->gui_attached || !(piece->request_histogram & DT_REQUEST_ONLY_IN_GUI))
         && (piece->request_histogram & DT_REQUEST_ON))
      {
        histogram_collect(piece, input, &roi_in, &(piece->histogram), piece->histogram_max);
        pixelpipe_flow |= (PIXELPIPE_FLOW_HISTOGRAM_ON_CPU);
        pixelpipe_flow &= ~(PIXELPIPE_FLOW_HISTOGRAM_NONE | PIXELPIPE_FLOW_HISTOGRAM_ON_GPU);

        if(piece->histogram && (module->request_histogram & DT_REQUEST_ON)
           && pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
        {
          const size_t buf_size = 4 * piece->histogram_stats.bins_count * sizeof(uint32_t);
          module->histogram = realloc(module->histogram, buf_size);
          memcpy(module->histogram, piece->histogram, buf_size);
          module->histogram_stats = piece->histogram_stats;
          memcpy(module->histogram_max, piece->histogram_max, sizeof(piece->histogram_max));

          dt_pthread_mutex_unlock(&pipe->busy_mutex);

          if(module->widget) dt_control_queue_redraw_widget(module->widget);

          dt_pthread_mutex_lock(&pipe->busy_mutex);
        }
      }

      if(pipe->shutdown)
      {
        dt_pthread_mutex_unlock(&pipe->busy_mutex);
        return 1;
      }

      /* process module on cpu. use tiling if needed and possible. */
      if(piece->process_tiling_ready
         && !dt_tiling_piece_fits_host_memory(MAX(roi_in.width, roi_out->width),
                                              MAX(roi_in.height, roi_out->height), MAX(in_bpp, bpp),
                                              tiling.factor, tiling.overhead))
      {
        module->process_tiling(module, piece, input, *output, &roi_in, roi_out, in_bpp);
        pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
        pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU);
      }
      else
      {
        module->process(module, piece, input, *output, &roi_in, roi_out);
        pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU);
        pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
      }

      if(pipe->shutdown)
      {
        dt_pthread_mutex_unlock(&pipe->busy_mutex);
        return 1;
      }

      // Lab color picking for module
      if(dev->gui_attached && pipe == dev->preview_pipe
         &&                           // pick from preview pipe to get pixels outside the viewport
         module == dev->gui_module && // only modules with focus can pick
         module->request_color_pick != DT_REQUEST_COLORPICK_OFF) // and they want to pick ;)
      {
        pixelpipe_picker(module, &piece->dsc_in, (float *)input, &roi_in, module->picked_color,
                         module->picked_color_min, module->picked_color_max, PIXELPIPE_PICKER_INPUT);
        pixelpipe_picker(module, &pipe->dsc, (float *)(*output), roi_out, module->picked_output_color,
                         module->picked_output_color_min, module->picked_output_color_max,
                         PIXELPIPE_PICKER_OUTPUT);

        dt_pthread_mutex_unlock(&pipe->busy_mutex);

        if(module->widget) dt_control_queue_redraw_widget(module->widget);

        dt_pthread_mutex_lock(&pipe->busy_mutex);
      }

      if(pipe->shutdown)
      {
        dt_pthread_mutex_unlock(&pipe->busy_mutex);
        return 1;
      }

      /* process blending */
      dt_develop_blend_process(module, piece, input, *output, &roi_in, roi_out);
      pixelpipe_flow |= (PIXELPIPE_FLOW_BLENDED_ON_CPU);
      pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_GPU);
    }
#else // HAVE_OPENCL
    // histogram collection for module
    if((dev->gui_attached || !(piece->request_histogram & DT_REQUEST_ONLY_IN_GUI))
       && (piece->request_histogram & DT_REQUEST_ON))
    {
      histogram_collect(piece, (float *)input, &roi_in, &(piece->histogram), piece->histogram_max);
      pixelpipe_flow |= (PIXELPIPE_FLOW_HISTOGRAM_ON_CPU);
      pixelpipe_flow &= ~(PIXELPIPE_FLOW_HISTOGRAM_NONE | PIXELPIPE_FLOW_HISTOGRAM_ON_GPU);

      if(piece->histogram && (module->request_histogram & DT_REQUEST_ON)
         && pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
      {
        const size_t buf_size = 4 * piece->histogram_stats.bins_count * sizeof(uint32_t);
        module->histogram = realloc(module->histogram, buf_size);
        memcpy(module->histogram, piece->histogram, buf_size);
        module->histogram_stats = piece->histogram_stats;
        memcpy(module->histogram_max, piece->histogram_max, sizeof(piece->histogram_max));

        dt_pthread_mutex_unlock(&pipe->busy_mutex);

        if(module->widget) dt_control_queue_redraw_widget(module->widget);

        dt_pthread_mutex_lock(&pipe->busy_mutex);
      }
    }

    if(pipe->shutdown)
    {
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      return 1;
    }

    /* process module on cpu. use tiling if needed and possible. */
    if(piece->process_tiling_ready
       && !dt_tiling_piece_fits_host_memory(MAX(roi_in.width, roi_out->width),
                                            MAX(roi_in.height, roi_out->height), MAX(in_bpp, bpp),
                                            tiling.factor, tiling.overhead))
    {
      module->process_tiling(module, piece, input, *output, &roi_in, roi_out, in_bpp);
      pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
      pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU);
    }
    else
    {
      module->process(module, piece, input, *output, &roi_in, roi_out);
      pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU);
      pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
    }

    if(pipe->shutdown)
    {
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      return 1;
    }

    // Lab color picking for module
    if(dev->gui_attached && pipe == dev->preview_pipe
       &&                           // pick from preview pipe to get pixels outside the viewport
       module == dev->gui_module && // only modules with focus can pick
       module->request_color_pick != DT_REQUEST_COLORPICK_OFF) // and they want to pick ;)
    {
      pixelpipe_picker(module, &piece->dsc_in, (float *)input, &roi_in, module->picked_color,
                       module->picked_color_min, module->picked_color_max, PIXELPIPE_PICKER_INPUT);
      pixelpipe_picker(module, &pipe->dsc, (float *)(*output), roi_out, module->picked_output_color,
                       module->picked_output_color_min, module->picked_output_color_max, PIXELPIPE_PICKER_OUTPUT);

      dt_pthread_mutex_unlock(&pipe->busy_mutex);

      if(module->widget) dt_control_queue_redraw_widget(module->widget);

      dt_pthread_mutex_lock(&pipe->busy_mutex);
    }

    if(pipe->shutdown)
    {
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      return 1;
    }

    /* process blending */
    dt_develop_blend_process(module, piece, input, *output, &roi_in, roi_out);
    pixelpipe_flow |= (PIXELPIPE_FLOW_BLENDED_ON_CPU);
    pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_GPU);
#endif // HAVE_OPENCL

    char histogram_log[32] = "";
    if(!(pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_NONE))
    {
      snprintf(histogram_log, sizeof(histogram_log), ", collected histogram on %s",
               (pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_ON_GPU
                    ? "GPU"
                    : pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_ON_CPU ? "CPU" : ""));
    }

    gchar *module_label = dt_history_item_get_name(module);
    dt_show_times(
        &start, "[dev_pixelpipe]", "processed `%s' on %s%s%s, blended on %s [%s]", module_label,
        pixelpipe_flow & PIXELPIPE_FLOW_PROCESSED_ON_GPU
            ? "GPU"
            : pixelpipe_flow & PIXELPIPE_FLOW_PROCESSED_ON_CPU ? "CPU" : "",
        pixelpipe_flow & PIXELPIPE_FLOW_PROCESSED_WITH_TILING ? " with tiling" : "",
        (!(pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_NONE) && (piece->request_histogram & DT_REQUEST_ON))
            ? histogram_log
            : "",
        pixelpipe_flow & PIXELPIPE_FLOW_BLENDED_ON_GPU
            ? "GPU"
            : pixelpipe_flow & PIXELPIPE_FLOW_BLENDED_ON_CPU ? "CPU" : "",
        _pipe_type_to_str(pipe->type));
    g_free(module_label);
    module_label = NULL;

    // in case we get this buffer from the cache in the future, cache some stuff:
    **out_format = piece->dsc_out = pipe->dsc;

    dt_pthread_mutex_unlock(&pipe->busy_mutex);
    if(module == darktable.develop->gui_module)
    {
      // give the input buffer to the currently focussed plugin more weight.
      // the user is likely to change that one soon, so keep it in cache.
      dt_dev_pixelpipe_cache_reweight(&(pipe->cache), input);
    }
#ifndef _DEBUG
    if(darktable.unmuted & DT_DEBUG_NAN)
#endif
    {
      dt_pthread_mutex_lock(&pipe->busy_mutex);
      if(pipe->shutdown)
      {
        dt_pthread_mutex_unlock(&pipe->busy_mutex);
        return 1;
      }

      if(strcmp(module->op, "gamma") == 0)
      {
        dt_pthread_mutex_unlock(&pipe->busy_mutex);
        goto post_process_collect_info;
      }

#ifdef HAVE_OPENCL
      if(*cl_mem_output != NULL)
        dt_opencl_copy_device_to_host(pipe->devid, *output, *cl_mem_output, roi_out->width, roi_out->height, bpp);
#endif

      if((*out_format)->datatype == TYPE_FLOAT && (*out_format)->channels == 4)
      {
        int hasinf = 0, hasnan = 0;
        float min[3] = { FLT_MAX };
        float max[3] = { FLT_MIN };

        for(int k = 0; k < 4 * roi_out->width * roi_out->height; k++)
        {
          if((k & 3) < 3)
          {
            float f = ((float *)(*output))[k];
            if(isnan(f))
              hasnan = 1;
            else if(isinf(f))
              hasinf = 1;
            else
            {
              min[k & 3] = fmin(f, min[k & 3]);
              max[k & 3] = fmax(f, max[k & 3]);
            }
          }
        }
        module_label = dt_history_item_get_name(module);
        if(hasnan)
          fprintf(stderr, "[dev_pixelpipe] module `%s' outputs NaNs! [%s]\n", module_label,
                  _pipe_type_to_str(pipe->type));
        if(hasinf)
          fprintf(stderr, "[dev_pixelpipe] module `%s' outputs non-finite floats! [%s]\n", module_label,
                  _pipe_type_to_str(pipe->type));
        fprintf(stderr, "[dev_pixelpipe] module `%s' min: (%f; %f; %f) max: (%f; %f; %f) [%s]\n", module_label,
                min[0], min[1], min[2], max[0], max[1], max[2], _pipe_type_to_str(pipe->type));
        g_free(module_label);
      }
      else if((*out_format)->datatype == TYPE_FLOAT && (*out_format)->channels == 1)
      {
        int hasinf = 0, hasnan = 0;
        float min = FLT_MAX;
        float max = FLT_MIN;

        for(int k = 0; k < roi_out->width * roi_out->height; k++)
        {
          float f = ((float *)(*output))[k];
          if(isnan(f))
            hasnan = 1;
          else if(isinf(f))
            hasinf = 1;
          else
          {
            min = fmin(f, min);
            max = fmax(f, max);
          }
        }
        module_label = dt_history_item_get_name(module);
        if(hasnan)
          fprintf(stderr, "[dev_pixelpipe] module `%s' outputs NaNs! [%s]\n", module_label,
                  _pipe_type_to_str(pipe->type));
        if(hasinf)
          fprintf(stderr, "[dev_pixelpipe] module `%s' outputs non-finite floats! [%s]\n", module_label,
                  _pipe_type_to_str(pipe->type));
        fprintf(stderr, "[dev_pixelpipe] module `%s' min: (%f) max: (%f) [%s]\n", module_label, min, max,
                _pipe_type_to_str(pipe->type));
        g_free(module_label);
      }

      dt_pthread_mutex_unlock(&pipe->busy_mutex);
    }

post_process_collect_info:

    dt_pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown)
    {
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      return 1;
    }
    // Picking RGB for the live samples and converting to Lab
    if(dev->gui_attached && pipe == dev->preview_pipe && (strcmp(module->op, "gamma") == 0)
       && darktable.lib->proxy.colorpicker.live_samples) // samples to pick
    {
      dt_colorpicker_sample_t *sample = NULL;
      GSList *samples = darktable.lib->proxy.colorpicker.live_samples;

      cmsHPROFILE Lab = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;

      if(darktable.color_profiles->display_type == DT_COLORSPACE_DISPLAY)
        pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);

      cmsHPROFILE out_profile = dt_colorspaces_get_profile(darktable.color_profiles->display_type,
                                                           darktable.color_profiles->display_filename,
                                                           DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY)->profile;

      cmsHTRANSFORM xform = out_profile ? cmsCreateTransform(out_profile, TYPE_RGB_FLT, Lab, TYPE_Lab_FLT, INTENT_PERCEPTUAL, 0) : NULL;

      if(darktable.color_profiles->display_type == DT_COLORSPACE_DISPLAY)
        pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

      while(samples)
      {
        sample = samples->data;

        if(sample->locked)
        {
          samples = g_slist_next(samples);
          continue;
        }

        uint8_t *pixel = (uint8_t *)*output;

        for(int k = 0; k < 3; k++) sample->picked_color_rgb_min[k] = 255;
        for(int k = 0; k < 3; k++) sample->picked_color_rgb_max[k] = 0;
        int box[4];
        int point[2];
        float rgb[3];
        for(int k = 0; k < 3; k++) rgb[k] = 0.0f;
        for(int k = 0; k < 4; k += 2)
          box[k] = MIN(roi_out->width - 1, MAX(0, sample->box[k] * roi_out->width));
        for(int k = 1; k < 4; k += 2)
          box[k] = MIN(roi_out->height - 1, MAX(0, sample->box[k] * roi_out->height));
        point[0] = MIN(roi_out->width - 1, MAX(0, sample->point[0] * roi_out->width));
        point[1] = MIN(roi_out->height - 1, MAX(0, sample->point[1] * roi_out->height));
        const float w = 1.0 / ((box[3] - box[1] + 1) * (box[2] - box[0] + 1));
        if(sample->size == DT_COLORPICKER_SIZE_BOX)
        {
          for(int j = box[1]; j <= box[3]; j++)
            for(int i = box[0]; i <= box[2]; i++)
            {
              for(int k = 0; k < 3; k++)
              {
                sample->picked_color_rgb_min[k]
                    = MIN(sample->picked_color_rgb_min[k], pixel[4 * (roi_out->width * j + i) + 2 - k]);
                sample->picked_color_rgb_max[k]
                    = MAX(sample->picked_color_rgb_max[k], pixel[4 * (roi_out->width * j + i) + 2 - k]);
                rgb[k] += w * pixel[4 * (roi_out->width * j + i) + 2 - k];
              }
            }
          for(int k = 0; k < 3; k++) sample->picked_color_rgb_mean[k] = rgb[k];
        }
        else
        {
          for(int i = 0; i < 3; i++)
            sample->picked_color_rgb_mean[i] = sample->picked_color_rgb_min[i]
                = sample->picked_color_rgb_max[i] = pixel[4 * (roi_out->width * point[1] + point[0]) + 2 - i];
        }

        // Converting the RGB values to Lab
        if(xform)
        {
          // Preparing the data for transformation
          float rgb_data[9];
          for(int i = 0; i < 3; i++)
          {
            rgb_data[i] = sample->picked_color_rgb_mean[i] / 255.0;
            rgb_data[i + 3] = sample->picked_color_rgb_min[i] / 255.0;
            rgb_data[i + 6] = sample->picked_color_rgb_max[i] / 255.0;
          }

          float Lab_data[9];
          cmsDoTransform(xform, rgb_data, Lab_data, 3);

          for(int i = 0; i < 3; i++)
          {
            sample->picked_color_lab_mean[i] = Lab_data[i];
            sample->picked_color_lab_min[i] = Lab_data[i + 3];
            sample->picked_color_lab_max[i] = Lab_data[i + 6];
          }
        }
        samples = g_slist_next(samples);
      }

      cmsDeleteTransform(xform);
    }
    // Picking RGB for primary colorpicker output and converting to Lab
    if(dev->gui_attached && pipe == dev->preview_pipe
       && (strcmp(module->op, "gamma") == 0) // only gamma provides meaningful RGB data
       && dev->gui_module && !strcmp(dev->gui_module->op, "colorout")
       && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF
       && darktable.lib->proxy.colorpicker.picked_color_rgb_mean) // colorpicker module active
    {
      uint8_t *pixel = (uint8_t *)*output;

      for(int k = 0; k < 3; k++) darktable.lib->proxy.colorpicker.picked_color_rgb_min[k] = 255;
      for(int k = 0; k < 3; k++) darktable.lib->proxy.colorpicker.picked_color_rgb_max[k] = 0;
      int box[4];
      int point[2];
      float rgb[3];
      for(int k = 0; k < 3; k++) rgb[k] = 0.0f;
      for(int k = 0; k < 4; k += 2)
        box[k] = MIN(roi_out->width - 1, MAX(0, dev->gui_module->color_picker_box[k] * roi_out->width));
      for(int k = 1; k < 4; k += 2)
        box[k] = MIN(roi_out->height - 1, MAX(0, dev->gui_module->color_picker_box[k] * roi_out->height));
      point[0] = MIN(roi_out->width - 1, MAX(0, dev->gui_module->color_picker_point[0] * roi_out->width));
      point[1] = MIN(roi_out->height - 1, MAX(0, dev->gui_module->color_picker_point[1] * roi_out->height));
      const float w = 1.0 / ((box[3] - box[1] + 1) * (box[2] - box[0] + 1));
      if(darktable.lib->proxy.colorpicker.size)
      {
        for(int j = box[1]; j <= box[3]; j++)
          for(int i = box[0]; i <= box[2]; i++)
          {
            for(int k = 0; k < 3; k++)
            {
              darktable.lib->proxy.colorpicker.picked_color_rgb_min[k]
                  = MIN(darktable.lib->proxy.colorpicker.picked_color_rgb_min[k],
                        pixel[4 * (roi_out->width * j + i) + 2 - k]);
              darktable.lib->proxy.colorpicker.picked_color_rgb_max[k]
                  = MAX(darktable.lib->proxy.colorpicker.picked_color_rgb_max[k],
                        pixel[4 * (roi_out->width * j + i) + 2 - k]);
              rgb[k] += w * pixel[4 * (roi_out->width * j + i) + 2 - k];
            }
          }
        for(int k = 0; k < 3; k++) darktable.lib->proxy.colorpicker.picked_color_rgb_mean[k] = rgb[k];
      }
      else
      {
        for(int i = 0; i < 3; i++)
          darktable.lib->proxy.colorpicker.picked_color_rgb_mean[i]
              = darktable.lib->proxy.colorpicker.picked_color_rgb_min[i]
              = darktable.lib->proxy.colorpicker.picked_color_rgb_max[i]
              = pixel[4 * (roi_out->width * point[1] + point[0]) + 2 - i];
      }

      // Converting the RGB values to Lab
      if(darktable.color_profiles->display_type == DT_COLORSPACE_DISPLAY)
        pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);

      cmsHPROFILE out_profile = dt_colorspaces_get_profile(darktable.color_profiles->display_type,
                                                           darktable.color_profiles->display_filename,
                                                           DT_PROFILE_DIRECTION_OUT | DT_PROFILE_DIRECTION_DISPLAY)->profile;

      if(out_profile)
      {
        cmsHPROFILE Lab = dt_colorspaces_get_profile(DT_COLORSPACE_LAB, "", DT_PROFILE_DIRECTION_ANY)->profile;

        cmsHTRANSFORM xform = cmsCreateTransform(out_profile, TYPE_RGB_FLT, Lab, TYPE_Lab_FLT, INTENT_PERCEPTUAL, 0);


        // Preparing the data for transformation
        float rgb_data[9];
        for(int i = 0; i < 3; i++)
        {
          rgb_data[i] = darktable.lib->proxy.colorpicker.picked_color_rgb_mean[i] / 255.0;
          rgb_data[i + 3] = darktable.lib->proxy.colorpicker.picked_color_rgb_min[i] / 255.0;
          rgb_data[i + 6] = darktable.lib->proxy.colorpicker.picked_color_rgb_max[i] / 255.0;
        }

        float Lab_data[9];
        cmsDoTransform(xform, rgb_data, Lab_data, 3);

        for(int i = 0; i < 3; i++)
        {
          darktable.lib->proxy.colorpicker.picked_color_lab_mean[i] = Lab_data[i];
          darktable.lib->proxy.colorpicker.picked_color_lab_min[i] = Lab_data[i + 3];
          darktable.lib->proxy.colorpicker.picked_color_lab_max[i] = Lab_data[i + 6];
        }

        cmsDeleteTransform(xform);
      }

      if(darktable.color_profiles->display_type == DT_COLORSPACE_DISPLAY)
        pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);


      dt_pthread_mutex_unlock(&pipe->busy_mutex);

      if(module->widget) dt_control_queue_redraw_widget(module->widget);
    }
    else
      dt_pthread_mutex_unlock(&pipe->busy_mutex);


    // 4) final histogram:
    dt_pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown)
    {
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      return 1;
    }
    if(dev->gui_attached && !dev->gui_leaving && pipe == dev->preview_pipe
       && (strcmp(module->op, "gamma") == 0))
    {
      float box[4];
      // Constraining the area if the colorpicker is active in area mode
      if(dev->gui_module && !strcmp(dev->gui_module->op, "colorout")
         && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF
         && darktable.lib->proxy.colorpicker.restrict_histogram)
      {
        if(darktable.lib->proxy.colorpicker.size == DT_COLORPICKER_SIZE_BOX)
        {
          for(int k = 0; k < 4; k += 2)
            box[k] = MIN(roi_out->width - 1,
                         MAX(0, dev->gui_module->color_picker_box[k] * (roi_out->width - 1)));
          for(int k = 1; k < 4; k += 2)
            box[k] = MIN(roi_out->height - 1,
                         MAX(0, dev->gui_module->color_picker_box[k] * (roi_out->height - 1)));
        }
        else
        {
          for(int k = 0; k < 4; k += 2)
            box[k] = MIN(roi_out->width - 1,
                         MAX(0, dev->gui_module->color_picker_point[0] * (roi_out->width - 1)));
          for(int k = 1; k < 4; k += 2)
            box[k] = MIN(roi_out->height - 1,
                         MAX(0, dev->gui_module->color_picker_point[1] * (roi_out->height - 1)));
        }
      }
      else
      {
        box[0] = box[1] = 0;
        box[2] = roi_out->width - 1;
        box[3] = roi_out->height - 1;
      }
      dev->histogram_max = 0;
      memset(dev->histogram, 0, sizeof(uint32_t) * 4 * 256);

      {
        uint8_t *pixel = (uint8_t *)*output;
        for(int j = box[1]; j <= box[3]; j += 4)
          for(int i = box[0]; i <= box[2]; i += 4)
          {
            uint8_t rgb[3];
            for(int k = 0; k < 3; k++) rgb[k] = pixel[4 * j * roi_out->width + 4 * i + 2 - k];

            for(int k = 0; k < 3; k++) dev->histogram[4 * rgb[k] + k]++;
            uint8_t lum = MAX(MAX(rgb[0], rgb[1]), rgb[2]);
            dev->histogram[4 * lum + 3]++;
          }
      }

      // don't count <= 0 pixels
      for(int k = 19; k < 4 * 256; k += 4)
        dev->histogram_max = dev->histogram_max > dev->histogram[k] ? dev->histogram_max : dev->histogram[k];

      // calculate the waveform histogram. since this is drawn pixel by pixel we have to do it in the correct
      // size (thus the weird gui stuff :().
      // this HAS to be done on the float input data, otherwise we get really ugly artifacts due to rounding
      // issues when putting colors into the bins.
      //       dt_pthread_mutex_lock(&dev->histogram_waveform_mutex);
      if(dev->histogram_waveform_width != 0 && input)
      {
        uint32_t *buf = (uint32_t *)calloc(dev->histogram_waveform_height * dev->histogram_waveform_width * 3,
                                           sizeof(uint32_t));
        memset(dev->histogram_waveform, 0,
               sizeof(uint32_t) * dev->histogram_waveform_height * dev->histogram_waveform_stride / 4);

        // 1.0 is at 8/9 of the height!
        const double bin_width = (double)(roi_in.width) / (double)dev->histogram_waveform_width,
                     _height = (double)(dev->histogram_waveform_height - 1);
        float *pixel = (float *)input;
        //         uint32_t mincol[3] = {UINT32_MAX,UINT32_MAX,UINT32_MAX}, maxcol[3] = {0,0,0};

        // count the colors into buf ...
        for(int y = 0; y < roi_in.height; y++)
        {
          for(int x = 0; x < roi_in.width; x++)
          {
            float rgb[3];
            for(int k = 0; k < 3; k++) rgb[k] = pixel[4 * y * roi_in.width + 4 * x + 2 - k];

            const int out_x = MIN(x / bin_width, dev->histogram_waveform_width - 1);
            for(int k = 0; k < 3; k++)
            {
              const float v = isnan(rgb[k]) ? 0.0f
                                            : rgb[k]; // catch NaNs as they don't convert well to integers
              const int out_y = CLAMP(1.0 - (8.0 / 9.0) * v, 0.0, 1.0) * _height;
              uint32_t *const out = buf + (out_y * dev->histogram_waveform_width * 3 + out_x * 3 + k);
              (*out)++;
              //               mincol[k] = MIN(mincol[k], *out);
              //               maxcol[k] = MAX(maxcol[k], *out);
            }
          }
        }

        // TODO: Find a nicer function to map buf -> image than just clipping
        //         float factor[3];
        //         for(int k = 0; k < 3; k++)
        //           factor[k] = 255.0 / (float)(maxcol[k] - mincol[k]); // leave some clipping

        // ... and scale that into a nice image. putting the pixels into the image directly gets too
        // saturated/clips.
        // new scale factor to do about the same as the old one for 1MP views, but scale to hidpi
        const float scale = 0.5 * 1e6f/(roi_in.height*roi_in.width) *
          (dev->histogram_waveform_width*dev->histogram_waveform_height) / (350.0f*233.);
        for(int y = 0; y < dev->histogram_waveform_height; y++)
        {
          for(int x = 0; x < dev->histogram_waveform_width; x++)
          {
            uint32_t *const in = buf + (y * dev->histogram_waveform_width + x) * 3;
            uint8_t *const out
                = (uint8_t *)(dev->histogram_waveform + (y * dev->histogram_waveform_width + x));
            for(int k = 0; k < 3; k++)
            {
              if(in[k] == 0) continue;
              out[k] = CLAMP(in[k] * scale, 5, 255);
              //               if(in[k] == 0)
              //                 out[k] = 0;
              //               else
              //                 out[k] = (float)(in[k] - mincol[k]) * factor[k];
            }
          }
        }

        free(buf);
      }
      //       dt_pthread_mutex_unlock(&dev->histogram_waveform_mutex);


      dt_pthread_mutex_unlock(&pipe->busy_mutex);

      /* raise preview pipe finished signal */
      dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED);
    }
    else
    {
      dt_pthread_mutex_unlock(&pipe->busy_mutex);

      /* if gui attached, lets raise pipe finish signal */
      if(dev->gui_attached && !dev->gui_leaving && strcmp(module->op, "gamma") == 0)
        dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED);
    }
  }

  return 0;
}


int dt_dev_pixelpipe_process_no_gamma(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int x, int y, int width,
                                      int height, float scale)
{
  // temporarily disable gamma mapping.
  GList *gammap = g_list_last(pipe->nodes);
  dt_dev_pixelpipe_iop_t *gamma = (dt_dev_pixelpipe_iop_t *)gammap->data;
  while(strcmp(gamma->module->op, "gamma"))
  {
    gamma = NULL;
    gammap = g_list_previous(gammap);
    if(!gammap) break;
    gamma = (dt_dev_pixelpipe_iop_t *)gammap->data;
  }
  if(gamma) gamma->enabled = 0;
  int ret = dt_dev_pixelpipe_process(pipe, dev, x, y, width, height, scale);
  if(gamma) gamma->enabled = 1;
  return ret;
}

void dt_dev_pixelpipe_disable_after(dt_dev_pixelpipe_t *pipe, const char *op)
{
  GList *nodes = g_list_last(pipe->nodes);
  dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
  while(strcmp(piece->module->op, op))
  {
    piece->enabled = 0;
    piece = NULL;
    nodes = g_list_previous(nodes);
    if(!nodes) break;
    piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
  }
}

void dt_dev_pixelpipe_disable_before(dt_dev_pixelpipe_t *pipe, const char *op)
{
  GList *nodes = pipe->nodes;
  dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
  while(strcmp(piece->module->op, op))
  {
    piece->enabled = 0;
    piece = NULL;
    nodes = g_list_next(nodes);
    if(!nodes) break;
    piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
  }
}

static int dt_dev_pixelpipe_process_rec_and_backcopy(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, void **output,
                                                     void **cl_mem_output, dt_iop_buffer_dsc_t **out_format,
                                                     const dt_iop_roi_t *roi_out, GList *modules, GList *pieces,
                                                     int pos)
{
#ifdef HAVE_OPENCL
  int ret
      = dt_dev_pixelpipe_process_rec(pipe, dev, output, cl_mem_output, out_format, roi_out, modules, pieces, pos);

  // copy back final opencl buffer (if any) to CPU
  dt_pthread_mutex_lock(&pipe->busy_mutex);
  if(ret)
  {
    dt_opencl_release_mem_object(*cl_mem_output);
    *cl_mem_output = NULL;
  }
  else
  {
    if(*cl_mem_output != NULL)
    {
      cl_int err;

      err = dt_opencl_copy_device_to_host(pipe->devid, *output, *cl_mem_output, roi_out->width, roi_out->height,
                                          dt_iop_buffer_dsc_to_bpp(*out_format));
      dt_opencl_release_mem_object(*cl_mem_output);
      *cl_mem_output = NULL;

      if(err != CL_SUCCESS)
      {
        /* this indicates a opencl problem earlier in the pipeline */
        dt_print(DT_DEBUG_OPENCL,
                 "[opencl_pixelpipe (d)] late opencl error detected while copying back to cpu buffer: %d\n",
                 err);
        pipe->opencl_error = 1;
        ret = 1;
      }
    }
  }
  dt_pthread_mutex_unlock(&pipe->busy_mutex);

  return ret;
#else
  return dt_dev_pixelpipe_process_rec(pipe, dev, output, cl_mem_output, out_format, roi_out, modules, pieces, pos);
#endif
}


int dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int x, int y, int width, int height,
                             float scale)
{
  pipe->processing = 1;
  pipe->opencl_enabled = dt_opencl_update_settings(); // update enabled flag and profile from preferences
  pipe->devid = (pipe->opencl_enabled) ? dt_opencl_lock_device(pipe->type)
                                       : -1; // try to get/lock opencl resource

  dt_print(DT_DEBUG_OPENCL, "[pixelpipe_process] [%s] using device %d\n", _pipe_type_to_str(pipe->type),
           pipe->devid);

  if(darktable.unmuted & DT_DEBUG_MEMORY)
  {
    fprintf(stderr, "[memory] before pixelpipe process\n");
    dt_print_mem_usage();
  }

  if(pipe->devid >= 0) dt_opencl_events_reset(pipe->devid);

  dt_iop_roi_t roi = (dt_iop_roi_t){ x, y, width, height, scale };
  // printf("pixelpipe homebrew process start\n");
  if(darktable.unmuted & DT_DEBUG_DEV) dt_dev_pixelpipe_cache_print(&pipe->cache);

  // get a snapshot of mask list
  if (pipe->forms) g_list_free_full(pipe->forms, (void (*)(void *))dt_masks_free_form);
  pipe->forms = dt_masks_dup_forms_deep(dev->forms, NULL);

  //  go through list of modules from the end:
  guint pos = g_list_length(pipe->iop);
  GList *modules = g_list_last(pipe->iop);
  GList *pieces = g_list_last(pipe->nodes);

// re-entry point: in case of late opencl errors we start all over again with opencl-support disabled
restart:

  // check if we should obsolete caches
  if(pipe->cache_obsolete) dt_dev_pixelpipe_cache_flush(&(pipe->cache));
  pipe->cache_obsolete = 0;

  // mask display off as a starting point
  pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;

  void *buf = NULL;
  void *cl_mem_out = NULL;

  dt_iop_buffer_dsc_t _out_format = { 0 };
  dt_iop_buffer_dsc_t *out_format = &_out_format;

  // run pixelpipe recursively and get error status
  int err = dt_dev_pixelpipe_process_rec_and_backcopy(pipe, dev, &buf, &cl_mem_out, &out_format, &roi, modules,
                                                      pieces, pos);

  // get status summary of opencl queue by checking the eventlist
  int oclerr = (pipe->devid >= 0) ? (dt_opencl_events_flush(pipe->devid, 1) != 0) : 0;

  // Check if we had opencl errors ....
  // remark: opencl errors can come in two ways: pipe->opencl_error is TRUE (and err is TRUE) OR oclerr is
  // TRUE
  if(oclerr || (err && pipe->opencl_error))
  {
    // Well, there were errors -> we might need to free an invalid opencl memory object
    dt_opencl_release_mem_object(cl_mem_out);
    dt_opencl_unlock_device(pipe->devid); // release opencl resource
    dt_pthread_mutex_lock(&pipe->busy_mutex);
    pipe->opencl_enabled = 0; // disable opencl for this pipe
    pipe->opencl_error = 0;   // reset error status
    pipe->devid = -1;
    dt_pthread_mutex_unlock(&pipe->busy_mutex);

    darktable.opencl->error_count++; // increase error count
    if(darktable.opencl->error_count >= DT_OPENCL_MAX_ERRORS)
    {
      // too frequent opencl errors encountered: this is a clear sign of a broken setup. give up on opencl
      // during this session.
      darktable.opencl->stopped = 1;
      dt_print(DT_DEBUG_OPENCL,
               "[opencl] frequent opencl errors encountered; disabling opencl for this session!\n");
      dt_control_log(
          _("darktable discovered problems with your OpenCL setup; disabling OpenCL for this session!"));
      // also remove "opencl" from capabilities so that the preference entry is greyed out
      dt_capabilities_remove("opencl");
    }

    dt_dev_pixelpipe_flush_caches(pipe);
    dt_dev_pixelpipe_change(pipe, dev);
    dt_print(DT_DEBUG_OPENCL, "[pixelpipe_process] [%s] falling back to cpu path\n",
             _pipe_type_to_str(pipe->type));
    goto restart; // try again (this time without opencl)
  }

  // release resources:
  if (pipe->forms)
  {
    g_list_free_full(pipe->forms, (void (*)(void *))dt_masks_free_form);
    pipe->forms = NULL;
  }
  if(pipe->devid >= 0)
  {
    dt_opencl_unlock_device(pipe->devid);
    pipe->devid = -1;
  }
  // ... and in case of other errors ...
  if(err)
  {
    pipe->processing = 0;
    return 1;
  }

  // terminate
  dt_pthread_mutex_lock(&pipe->backbuf_mutex);
  pipe->backbuf_hash = dt_dev_pixelpipe_cache_hash(pipe->image.id, &roi, pipe, 0);
  pipe->backbuf = buf;
  pipe->backbuf_width = width;
  pipe->backbuf_height = height;
  dt_pthread_mutex_unlock(&pipe->backbuf_mutex);

  // printf("pixelpipe homebrew process end\n");
  pipe->processing = 0;
  return 0;
}

void dt_dev_pixelpipe_flush_caches(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_flush(&pipe->cache);
}

void dt_dev_pixelpipe_get_dimensions(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, int width_in,
                                     int height_in, int *width, int *height)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex);
  dt_iop_roi_t roi_in = (dt_iop_roi_t){ 0, 0, width_in, height_in, 1.0 };
  dt_iop_roi_t roi_out;
  GList *modules = pipe->iop;
  GList *pieces = pipe->nodes;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)pieces->data;

    piece->buf_in = roi_in;

    // skip this module?
    if(piece->enabled
       && !(dev->gui_module && dev->gui_module->operation_tags_filter() & module->operation_tags()))
    {
      module->modify_roi_out(module, piece, &roi_out, &roi_in);
    }
    else
    {
      // pass through regions of interest for gui post expose events
      roi_out = roi_in;
    }

    piece->buf_out = roi_out;
    roi_in = roi_out;

    modules = g_list_next(modules);
    pieces = g_list_next(pieces);
  }
  *width = roi_out.width;
  *height = roi_out.height;
  dt_pthread_mutex_unlock(&pipe->busy_mutex);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
