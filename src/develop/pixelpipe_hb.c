/*
    This file is part of darktable,
    Copyright (C) 2009-2023 darktable developers.

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
#include "common/opencl.h"
#include "common/iop_order.h"
#include "common/imagebuf.h"
#include "control/control.h"
#include "control/signal.h"
#include "develop/blend.h"
#include "develop/format.h"
#include "develop/imageop_math.h"
#include "develop/develop.h"
#include "develop/tiling.h"
#include "develop/masks.h"
#include "gui/gtk.h"
#include "imageio/imageio_common.h"
#include "libs/colorpicker.h"
#include "libs/lib.h"
#include "gui/color_picker_proxy.h"

#include <assert.h>
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

const char *dt_dev_pixelpipe_type_to_str(const int pipe_type)
{
  const gboolean fast = pipe_type & DT_DEV_PIXELPIPE_FAST;
  const char *r = NULL;

  switch(pipe_type & DT_DEV_PIXELPIPE_ANY)
  {
    case DT_DEV_PIXELPIPE_PREVIEW:
      if(fast)
        r = "preview/fast";
      else
        r = "preview";
      break;
    case DT_DEV_PIXELPIPE_PREVIEW2:
      if(fast)
        r = "preview2/fast";
      else
        r = "preview2";
      break;
    case DT_DEV_PIXELPIPE_FULL:
      r = "full";
      break;
    case DT_DEV_PIXELPIPE_THUMBNAIL:
      if(fast)
        r = "thumbnail/fast";
      else
        r = "thumbnail";
      break;
    case DT_DEV_PIXELPIPE_EXPORT:
      if(fast)
        r = "export/fast";
      else
        r = "export";
      break;
    default:
      r = "unknown";
  }
  return r;
}

void dt_print_pipe(dt_debug_thread_t thread,
                   const char *title,
                   dt_dev_pixelpipe_t *pipe,
                   const struct dt_iop_module_t *module,
                   const dt_iop_roi_t *roi_in,
                   const dt_iop_roi_t *roi_out,
                   const char *msg, ...)
{
  if(thread != DT_DEBUG_ALWAYS)
  {
    if(((darktable.unmuted & thread) & ~DT_DEBUG_VERBOSE) == 0) return;
    if((thread & DT_DEBUG_VERBOSE) && !(darktable.unmuted & DT_DEBUG_VERBOSE)) return;
  }
  if(pipe
      && !(thread & DT_DEBUG_VERBOSE)
      && (pipe->type & (DT_DEV_PIXELPIPE_PREVIEW | DT_DEV_PIXELPIPE_PREVIEW)) )
    return;

  char buf[3][128];
  char vbuf[1024] = { 0 };
  char roi[128] = { 0 };
  char roo[128] = { 0 };
  char pname[32] = { 0 };
  char masking[64] = { 0 };

  snprintf(buf[0], sizeof(buf[0]), "%.4f", dt_get_wtime() - darktable.start_wtime);
  snprintf(buf[1], sizeof(buf[1]), "[%s]", title);

  snprintf(buf[2], sizeof(buf[2]), "%s%s",
    module ? module->name() : "",
    module ? dt_iop_get_instance_name(module) : "");

  if(roi_in)
    snprintf(roi, sizeof(roi),
             "(%4i/%4i) %4ix%4i scale=%.4f",
             roi_in->x, roi_in->y, roi_in->width, roi_in->height, roi_in->scale);
  if(roi_out)
  {
    snprintf(roo, sizeof(roo),
             " --> (%4i/%4i) %4ix%4i scale=%.4f",
             roi_out->x, roi_out->y, roi_out->width, roi_out->height, roi_out->scale);
  }

  if(pipe)
  {
    snprintf(pname, sizeof(pname), "[%s]", dt_dev_pixelpipe_type_to_str(pipe->type));
    if(pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY)
      snprintf(masking, sizeof(masking),
               " masking=%#x %s", pipe->mask_display,
               pipe->bypass_blendif ? ", bypass blend" : "" );
  }

  va_list ap;
  va_start(ap, msg);
  vsnprintf(vbuf, sizeof(vbuf), msg, ap);
  va_end(ap);

  printf("%11s %-28s %-14s %-24s %s%s%s %s",
         buf[0], buf[1], pname, buf[2], roi, roo, masking, vbuf);
  fflush(stdout);
}

gboolean dt_dev_pixelpipe_init_export(dt_dev_pixelpipe_t *pipe,
                                      const int32_t width,
                                      const int32_t height,
                                      const int levels,
                                      const gboolean store_masks)
{
  const gboolean res =
    dt_dev_pixelpipe_init_cached(pipe, sizeof(float) * 4 * width * height, DT_PIPECACHE_MIN, 0);
  pipe->type = DT_DEV_PIXELPIPE_EXPORT;
  pipe->levels = levels;
  pipe->store_all_raster_masks = store_masks;
  return res;
}

gboolean dt_dev_pixelpipe_init_thumbnail(dt_dev_pixelpipe_t *pipe,
                                         const int32_t width,
                                         const int32_t height)
{
  const gboolean res =
    dt_dev_pixelpipe_init_cached(pipe, sizeof(float) * 4 * width * height, DT_PIPECACHE_MIN, 0);
  pipe->type = DT_DEV_PIXELPIPE_THUMBNAIL;
  return res;
}

gboolean dt_dev_pixelpipe_init_dummy(dt_dev_pixelpipe_t *pipe,
                                     const int32_t width,
                                     const int32_t height)
{
  const gboolean res =
    dt_dev_pixelpipe_init_cached(pipe, sizeof(float) * 4 * width * height, 0, 0);
  pipe->type = DT_DEV_PIXELPIPE_THUMBNAIL;
  return res;
}

gboolean dt_dev_pixelpipe_init_preview(dt_dev_pixelpipe_t *pipe)
{
  const gboolean res =
    dt_dev_pixelpipe_init_cached(pipe, 0, darktable.pipe_cache ? 12 : DT_PIPECACHE_MIN, 0);
  pipe->type = DT_DEV_PIXELPIPE_PREVIEW;
  return res;
}

gboolean dt_dev_pixelpipe_init_preview2(dt_dev_pixelpipe_t *pipe)
{
  const gboolean res =
    dt_dev_pixelpipe_init_cached(pipe, 0, darktable.pipe_cache ? 5 : DT_PIPECACHE_MIN, 0);
  pipe->type = DT_DEV_PIXELPIPE_PREVIEW2;
  return res;
}

gboolean dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe)
{
  const size_t csize = MAX(64*1024*1024, darktable.dtresources.mipmap_memory / 4);
  const gboolean res =
    dt_dev_pixelpipe_init_cached(pipe, 0, darktable.pipe_cache ? 64 : DT_PIPECACHE_MIN, csize);
  pipe->type = DT_DEV_PIXELPIPE_FULL;
  return res;
}

gboolean dt_dev_pixelpipe_init_cached(dt_dev_pixelpipe_t *pipe,
                                      const size_t size,
                                      const int32_t entries,
                                      const size_t memlimit)
{
  pipe->devid = -1;
  pipe->changed = DT_DEV_PIPE_UNCHANGED;
  pipe->processed_width = pipe->backbuf_width = pipe->iwidth = pipe->final_width = 0;
  pipe->processed_height = pipe->backbuf_height = pipe->iheight = pipe->final_height = 0;
  pipe->nodes = NULL;
  pipe->backbuf_size = size;
  pipe->cache_obsolete = FALSE;
  pipe->backbuf = NULL;
  pipe->backbuf_scale = 0.0f;
  pipe->backbuf_zoom_x = 0.0f;
  pipe->backbuf_zoom_y = 0.0f;

  pipe->output_backbuf = NULL;
  pipe->output_backbuf_width = 0;
  pipe->output_backbuf_height = 0;
  pipe->output_imgid = NO_IMGID;

  pipe->rawdetail_mask_data = NULL;
  pipe->want_detail_mask = FALSE;
  pipe->detail_width = pipe->detail_height = 0;

  pipe->processing = FALSE;
  dt_atomic_set_int(&pipe->shutdown,FALSE);
  pipe->opencl_error = FALSE;
  pipe->tiling = FALSE;
  pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  pipe->bypass_blendif = FALSE;
  pipe->input_timestamp = 0;
  pipe->levels = IMAGEIO_RGB | IMAGEIO_INT8;
  dt_pthread_mutex_init(&(pipe->backbuf_mutex), NULL);
  dt_pthread_mutex_init(&(pipe->busy_mutex), NULL);
  pipe->icc_type = DT_COLORSPACE_NONE;
  pipe->icc_filename = NULL;
  pipe->icc_intent = DT_INTENT_LAST;
  pipe->iop = NULL;
  pipe->iop_order_list = NULL;
  pipe->forms = NULL;
  pipe->store_all_raster_masks = FALSE;
  pipe->work_profile_info = NULL;
  pipe->input_profile_info = NULL;
  pipe->output_profile_info = NULL;

  return dt_dev_pixelpipe_cache_init(pipe, entries, size, memlimit);
}

static void get_output_format(
              dt_iop_module_t *module,
              dt_dev_pixelpipe_t *pipe,
              dt_dev_pixelpipe_iop_t *piece,
              dt_develop_t *dev,
              dt_iop_buffer_dsc_t *dsc)
{
  if(module) return module->output_format(module, pipe, piece, dsc);

  // first input.
  *dsc = pipe->image.buf_dsc;

  if(!(dt_image_is_raw(&pipe->image)))
  {
    // image max is normalized before
    for(int k = 0; k < 4; k++) dsc->processed_maximum[k] = 1.0f;
  }
}

void dt_dev_pixelpipe_set_input(dt_dev_pixelpipe_t *pipe,
                                dt_develop_t *dev,
                                float *input,
                                const int width,
                                const int height,
                                const float iscale)
{
  pipe->iwidth = width;
  pipe->iheight = height;
  pipe->iscale = iscale;
  pipe->input = input;
  pipe->image = dev->image_storage;
  get_output_format(NULL, pipe, NULL, dev, &pipe->dsc);
}

void dt_dev_pixelpipe_set_icc(dt_dev_pixelpipe_t *pipe,
                              const dt_colorspaces_color_profile_type_t icc_type,
                              const gchar *icc_filename,
                              const dt_iop_color_intent_t icc_intent)
{
  pipe->icc_type = icc_type;
  g_free(pipe->icc_filename);
  pipe->icc_filename = g_strdup(icc_filename ? icc_filename : "");
  pipe->icc_intent = icc_intent;
}

void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe)
{
  dt_pthread_mutex_lock(&pipe->backbuf_mutex);
  pipe->backbuf = NULL;
  // blocks while busy and sets shutdown bit:
  dt_dev_pixelpipe_cleanup_nodes(pipe);
  // so now it's safe to clean up cache:
  dt_dev_pixelpipe_cache_cleanup(pipe);
  dt_pthread_mutex_unlock(&pipe->backbuf_mutex);

  dt_pthread_mutex_destroy(&(pipe->backbuf_mutex));
  dt_pthread_mutex_destroy(&(pipe->busy_mutex));
  pipe->icc_type = DT_COLORSPACE_NONE;
  g_free(pipe->icc_filename);
  pipe->icc_filename = NULL;

  g_free(pipe->output_backbuf);
  pipe->output_backbuf = NULL;
  pipe->output_backbuf_width = 0;
  pipe->output_backbuf_height = 0;
  pipe->output_imgid = NO_IMGID;
  pipe->next_important_module = FALSE;

  if(pipe->forms)
  {
    g_list_free_full(pipe->forms, (void (*)(void *))dt_masks_free_form);
    pipe->forms = NULL;
  }
}

void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe)
{
  dt_atomic_set_int(&pipe->shutdown,TRUE); // tell pipe that it should
                                           // shut itself down if
                                           // currently running

  // FIXME: either this or all process() -> gdk mutices have to be changed!
  //        (this is a circular dependency on busy_mutex and the gdk mutex)
  // [[does the above still apply?]]
  dt_pthread_mutex_lock(&pipe->busy_mutex); // block until the pipe has shut down
  // destroy all nodes
  for(GList *nodes = pipe->nodes; nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    // printf("cleanup module `%s'\n", piece->module->name());
    piece->module->cleanup_pipe(piece->module, pipe, piece);
    free(piece->blendop_data);
    piece->blendop_data = NULL;
    dt_free_align(piece->histogram);
    piece->histogram = NULL;
    g_hash_table_destroy(piece->raster_masks);
    piece->raster_masks = NULL;
    free(piece);
  }
  g_list_free(pipe->nodes);
  pipe->nodes = NULL;

  dt_dev_clear_rawdetail_mask(pipe);

  // also cleanup iop here
  if(pipe->iop)
  {
    g_list_free(pipe->iop);
    pipe->iop = NULL;
  }
  // and iop order
  g_list_free_full(pipe->iop_order_list, free);
  pipe->iop_order_list = NULL;
  dt_pthread_mutex_unlock(&pipe->busy_mutex);	// safe for others to mess with the pipe now
}

void dt_dev_pixelpipe_rebuild(dt_develop_t *dev)
{
  dev->pipe->changed |= DT_DEV_PIPE_REMOVE;
  dev->preview_pipe->changed |= DT_DEV_PIPE_REMOVE;
  dev->preview2_pipe->changed |= DT_DEV_PIPE_REMOVE;

  dev->pipe->cache_obsolete = TRUE;
  dev->preview_pipe->cache_obsolete = TRUE;
  dev->preview2_pipe->cache_obsolete = TRUE;

  // invalidate buffers and force redraw of darkroom
  dt_dev_invalidate_all(dev);
}

void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe,
                                   dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex); // block until pipe is idle
  // clear any pending shutdown request
  dt_atomic_set_int(&pipe->shutdown,FALSE);
  // check that the pipe was actually properly cleaned up after the last run
  g_assert(pipe->nodes == NULL);
  g_assert(pipe->iop == NULL);
  g_assert(pipe->iop_order_list == NULL);
  pipe->iop_order_list = dt_ioppr_iop_order_copy_deep(dev->iop_order_list);
  // for all modules in dev:
  pipe->iop = g_list_copy(dev->iop);
  for(GList *modules = pipe->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    dt_dev_pixelpipe_iop_t *piece =
      (dt_dev_pixelpipe_iop_t *)calloc(1, sizeof(dt_dev_pixelpipe_iop_t));
    piece->enabled = module->enabled;
    piece->request_histogram = DT_REQUEST_ONLY_IN_GUI;
    piece->histogram_params.roi = NULL;
    piece->histogram_params.bins_count = 256;
    piece->histogram_stats.bins_count = 0;
    piece->histogram_stats.pixels = 0;
    piece->colors
        = ((module->default_colorspace(module, pipe, NULL) == IOP_CS_RAW)
           && (dt_image_is_raw(&pipe->image)))
      ? 1
      : 4;
    piece->iscale = pipe->iscale;
    piece->iwidth = pipe->iwidth;
    piece->iheight = pipe->iheight;
    piece->module = module;
    piece->pipe = pipe;
    piece->data = NULL;
    piece->hash = 0;
    piece->process_cl_ready = FALSE;
    piece->process_tiling_ready = FALSE;
    piece->raster_masks = g_hash_table_new_full(g_direct_hash,
                                                g_direct_equal, NULL, dt_free_align_ptr);
    memset(&piece->processed_roi_in, 0, sizeof(piece->processed_roi_in));
    memset(&piece->processed_roi_out, 0, sizeof(piece->processed_roi_out));
    dt_iop_init_pipe(piece->module, pipe, piece);
    pipe->nodes = g_list_append(pipe->nodes, piece);
  }
  dt_pthread_mutex_unlock(&pipe->busy_mutex); // safe for others to
                                              // use/mess with the
                                              // pipe now
}

// helper
void dt_dev_pixelpipe_synch(dt_dev_pixelpipe_t *pipe,
                            dt_develop_t *dev,
                            GList *history)
{
  dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
  // find piece in nodes list
  dt_dev_pixelpipe_iop_t *piece = NULL;

  const dt_image_t *img      = &pipe->image;
  const dt_imgid_t imgid     = img->id;
  const gboolean rawprep_img = dt_image_is_rawprepare_supported(img);

  for(GList *nodes = pipe->nodes; nodes; nodes = g_list_next(nodes))
  {
    piece = (dt_dev_pixelpipe_iop_t *)nodes->data;

    if(piece->module == hist->module)
    {
      const gboolean active = hist->enabled;
      piece->enabled = active;

      // Styles, presets or history copy&paste might set history items
      // not appropriate for the image.  Fixing that seemed to be
      // almost impossible after long discussions but at least we can
      // test, correct and add a problem hint here.
      if((dt_iop_module_is(piece->module->so, "demosaic"))
         || (dt_iop_module_is(piece->module->so, "rawprepare")))
      {
        if(rawprep_img && !active)
          piece->enabled = TRUE;
        else if(!rawprep_img && active)
          piece->enabled = FALSE;
      }
      else if((dt_iop_module_is(piece->module->so, "rawdenoise"))
              || (dt_iop_module_is(piece->module->so, "hotpixels"))
              || (dt_iop_module_is(piece->module->so, "cacorrect")))
      {
        if(!rawprep_img && active) piece->enabled = FALSE;
      }

      if((piece->enabled && !hist->enabled)
         || (!piece->enabled && hist->enabled))
      {
        if(piece->enabled)
          dt_iop_set_module_trouble_message
            (piece->module,
             _("enabled as required"),
             _("history had module disabled but it is required for"
               " this type of image.\nlikely introduced by applying a preset,"
               " style or history copy&paste"),
             NULL);
        else
          dt_iop_set_module_trouble_message
            (piece->module,
             _("disabled as not appropriate"),
             _("history had module enabled but it is not allowed for this type"
               " of image.\nlikely introduced by applying a preset, style or"
               " history copy&paste"),
             NULL);

        dt_print_pipe(DT_DEBUG_PIPE, "pixelpipe synch problem",
          pipe, piece->module, NULL, NULL,
          "piece enabling mismatch for image %i, piece hash%22" PRIu64 ", \n",
          imgid, piece->hash); 
      }
      dt_iop_commit_params(hist->module, hist->params, hist->blend_params, pipe, piece);

      dt_print_pipe(DT_DEBUG_PIPE, "committed params",
          pipe, piece->module, NULL, NULL,
          "piece hash%22" PRIu64 ", \n", piece->hash); 

      if(piece->blendop_data)
      {
        const dt_develop_blend_params_t *const bp =
          (const dt_develop_blend_params_t *)piece->blendop_data;
        if(!feqf(bp->details, 0.0f, 1e-6))
          pipe->want_detail_mask = TRUE;
      }
    }
  }
}

void dt_dev_pixelpipe_synch_all(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex);

  dt_print(DT_DEBUG_PARAMS,
           "[pixelpipe] [%s] synch all modules with defaults_params\n",
           dt_dev_pixelpipe_type_to_str(pipe->type));

  // call reset_params on all pieces first. This is mandatory to init
  // utility modules that don't have an history stack
  for(GList *nodes = pipe->nodes; nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    piece->hash = 0;
    piece->enabled = piece->module->default_enabled;
    dt_iop_commit_params(piece->module,
                         piece->module->default_params,
                         piece->module->default_blendop_params,
                         pipe, piece);
  }

  dt_print(DT_DEBUG_PARAMS,
           "[pixelpipe] [%s] synch all modules with history\n",
           dt_dev_pixelpipe_type_to_str(pipe->type));

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
  if(history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
    dt_print(DT_DEBUG_PARAMS,
             "[pixelpipe] [%s] synch top history module `%s'\n",
             dt_dev_pixelpipe_type_to_str(pipe->type), hist->module->op);
    dt_dev_pixelpipe_synch(pipe, dev, history);
  }
  else
  {
    dt_print(DT_DEBUG_PARAMS,
             "[pixelpipe] [%s] synch top history module missing error\n",
             dt_dev_pixelpipe_type_to_str(pipe->type));
  }
  dt_pthread_mutex_unlock(&pipe->busy_mutex);
}

void dt_dev_pixelpipe_change(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&dev->history_mutex);

  dt_print(DT_DEBUG_PARAMS,
           "[pixelpipe] [%s] pipeline state changing, flag %i\n",
           dt_dev_pixelpipe_type_to_str(pipe->type), pipe->changed);
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
    // modules have been added in between or removed. need to rebuild
    // the whole pipeline.
    dt_dev_pixelpipe_cleanup_nodes(pipe);
    dt_dev_pixelpipe_create_nodes(pipe, dev);
    dt_dev_pixelpipe_synch_all(pipe, dev);
  }
  pipe->changed = DT_DEV_PIPE_UNCHANGED;
  dt_pthread_mutex_unlock(&dev->history_mutex);
  dt_dev_pixelpipe_get_dimensions(pipe, dev,
                                  pipe->iwidth, pipe->iheight,
                                  &pipe->processed_width,
                                  &pipe->processed_height);
}

static void _dump_pipe_pfm_diff(
        const char *mod,
        const void *indata,
        const dt_iop_roi_t *roi_in,
        const int inbpp,
        const void *outdata,
        const dt_iop_roi_t *roi_out,
        const int outbpp,
        const char *pipe)
{
  if(!darktable.dump_pfm_pipe) return;
  if(!mod) return;
  if(!dt_str_commasubstring(darktable.dump_pfm_pipe, mod)) return;
  if(inbpp != outbpp) return;
  if(!(inbpp == 16 || inbpp == 4)) return;


  const int fchannels = inbpp / 4;
  float *mixed = dt_alloc_align_float((size_t)fchannels * roi_out->width * roi_out->height);
  if(!mixed) return;

  const float *in = indata;
  const float *out = outdata;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(roi_in, roi_out, mixed, in, out, fchannels) \
  schedule(static) collapse(2)
#endif
  for(int row = 0; row < roi_out->height; row++)
  {
    for(int col = 0; col < roi_out->width; col++)
    {
      const size_t ox = fchannels * (row * roi_out->width + col);
      const int irow = row + roi_out->y;
      const int icol = col + roi_out->x;

      for(int c = 0; c < fchannels; c++)
      {
        if((irow < roi_in->height) && (icol < roi_in->width) && (icol >= 0) && (irow >= 0))
          mixed[ox+c] = fabsf(in[fchannels * (irow * roi_in->width + icol)+c] - out[ox+c]);
        else
          mixed[ox+c] = 0.0f;
      }
    }
  }
  dt_dump_pfm_file(pipe, mixed, roi_out->width, roi_out->height,
                   outbpp, mod, "[dt_dump_pipe_pfm]", TRUE, TRUE, TRUE);
  dt_free_align(mixed);
}

// helper to get per module histogram
static void _histogram_collect(dt_dev_pixelpipe_iop_t *piece,
                               const void *pixel,
                               const dt_iop_roi_t *roi,
                               uint32_t **histogram,
                               uint32_t *histogram_max)
{
  dt_dev_histogram_collection_params_t histogram_params = piece->histogram_params;

  dt_histogram_roi_t histogram_roi;

  // if the current module does did not specified its own ROI, use the full ROI
  if(histogram_params.roi == NULL)
  {
    histogram_roi = (dt_histogram_roi_t){
      .width = roi->width,
      .height = roi->height,
      .crop_x = 0,
      .crop_y = 0,
      .crop_right = 0,
      .crop_bottom = 0
    };

    histogram_params.roi = &histogram_roi;
  }

  const dt_iop_colorspace_type_t cst =
    piece->module->input_colorspace(piece->module, piece->pipe, piece);

  dt_histogram_helper(&histogram_params, &piece->histogram_stats, cst,
                      piece->module->histogram_cst,
                      pixel, histogram, histogram_max,
                      piece->module->histogram_middle_grey,
                      dt_ioppr_get_pipe_work_profile_info(piece->pipe));
}

#ifdef HAVE_OPENCL
// helper to get per module histogram for OpenCL
//
// this algorithm is inefficient as hell when it comes to larger
// images. it's only acceptable as long as we work on small image
// sizes like in image preview
static void _histogram_collect_cl(const int devid,
                                  dt_dev_pixelpipe_iop_t *piece,
                                  cl_mem img,
                                  const dt_iop_roi_t *roi,
                                  uint32_t **histogram,
                                  uint32_t *histogram_max,
                                  float *buffer,
                                  const size_t bufsize)
{
  float *tmpbuf = NULL;
  float *pixel = NULL;

  // if buffer is supplied and if size fits let's use it
  if(buffer && bufsize >= (size_t)roi->width * roi->height * 4 * sizeof(float))
    pixel = buffer;
  else
    pixel = tmpbuf = dt_alloc_align_float((size_t)4 * roi->width * roi->height);

  if(!pixel) return;

  const cl_int err = dt_opencl_copy_device_to_host(devid, pixel, img,
                                                   roi->width, roi->height,
                                                   sizeof(float) * 4);
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
      .width = roi->width,
      .height = roi->height,
      .crop_x = 0,
      .crop_y = 0,
      .crop_right = 0,
      .crop_bottom = 0
    };

    histogram_params.roi = &histogram_roi;
  }

  const dt_iop_colorspace_type_t cst =
    piece->module->input_colorspace(piece->module, piece->pipe, piece);

  dt_histogram_helper(&histogram_params, &piece->histogram_stats,
                      cst, piece->module->histogram_cst,
                      pixel, histogram, histogram_max,
                      piece->module->histogram_middle_grey,
                      dt_ioppr_get_pipe_work_profile_info(piece->pipe));

  if(tmpbuf) dt_free_align(tmpbuf);
}
#endif

// calculate box in current module's coordinates for the color picker
// FIXME: move this to common color picker code?
static int _pixelpipe_picker_box(dt_iop_module_t *module,
                                 const dt_iop_roi_t *roi,
                                 const dt_colorpicker_sample_t *const sample,
                                 dt_pixelpipe_picker_source_t picker_source,
                                 int *box)
{
  if(picker_source == PIXELPIPE_PICKER_OUTPUT
     && !sample->pick_output)
    return 1;

  const float wd = darktable.develop->preview_pipe->backbuf_width;
  const float ht = darktable.develop->preview_pipe->backbuf_height;
  const int width = roi->width;
  const int height = roi->height;
  const dt_image_t image = darktable.develop->image_storage;
  const int op_after_demosaic =
    dt_ioppr_is_iop_before(darktable.develop->preview_pipe->iop_order_list,
                           module->op, "demosaic", 0);

  dt_boundingbox_t fbox = { 0.0f };

  // get absolute pixel coordinates in final preview image
  if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
  {
    for(int k = 0; k < 4; k += 2)
      fbox[k] = sample->box[k] * wd;
    for(int k = 1; k < 4; k += 2)
      fbox[k] = sample->box[k] * ht;
  }
  else if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
  {
    fbox[0] = fbox[2] = sample->point[0] * wd;
    fbox[1] = fbox[3] = sample->point[1] * ht;
  }

  // transform back to current module coordinates
  dt_dev_distort_backtransform_plus
    (darktable.develop, darktable.develop->preview_pipe, module->iop_order,
     ((picker_source == PIXELPIPE_PICKER_INPUT)
      ? DT_DEV_TRANSFORM_DIR_FORW_INCL
      : DT_DEV_TRANSFORM_DIR_FORW_EXCL),
     fbox, 2);

  if(op_after_demosaic || !dt_image_is_rawprepare_supported(&image))
    for(int idx = 0; idx < 4; idx++)
      fbox[idx] *= darktable.develop->preview_downsampling;

  fbox[0] -= roi->x;
  fbox[1] -= roi->y;
  fbox[2] -= roi->x;
  fbox[3] -= roi->y;

  // re-order edges of bounding box
  box[0] = fminf(fbox[0], fbox[2]);
  box[1] = fminf(fbox[1], fbox[3]);
  box[2] = fmaxf(fbox[0], fbox[2]);
  box[3] = fmaxf(fbox[1], fbox[3]);

  // make sure we sample at least one point
  box[2] = fmaxf(box[2], box[0] + 1);
  box[3] = fmaxf(box[3], box[1] + 1);

  // do not continue if box is completely outside of roi
  // FIXME: on invalid box, caller should set sample to something like
  // NaN to flag it as invalid
  if(box[0] >= width
     || box[1] >= height
     || box[2] < 0
     || box[3] < 0)
    return 1;

  // clamp bounding box to roi
  box[0] = CLAMP(box[0], 0, width - 1);
  box[1] = CLAMP(box[1], 0, height - 1);
  box[2] = CLAMP(box[2], 1, width);
  box[3] = CLAMP(box[3], 1, height);

  // safety check: area needs to have minimum 1 pixel width and height
  if(box[2] - box[0] < 1
     || box[3] - box[1] < 1)
    return 1;

  return 0;
}

// color picking for module
// FIXME: make called with: lib_colorpicker_sample_statistics pick
static void _pixelpipe_picker(dt_iop_module_t *module,
                              dt_dev_pixelpipe_iop_t *piece,
                              dt_iop_buffer_dsc_t *dsc,
                              const float *pixel,
                              const dt_iop_roi_t *roi,
                              float *picked_color,
                              float *picked_color_min,
                              float *picked_color_max,
                              const dt_iop_colorspace_type_t image_cst,
                              const dt_pixelpipe_picker_source_t picker_source)
{
  int box[4] = { 0 };

  // FIXME: don't need to initialize this if dt_color_picker_helper() does
  lib_colorpicker_stats pick =
    { { 0.0f, 0.0f, 0.0f, 0.0f },
      { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX },
      { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX } };

  if(!_pixelpipe_picker_box(module, roi,
                            darktable.lib->proxy.colorpicker.primary_sample,
                            picker_source, box))
  {
    const dt_iop_order_iccprofile_info_t *const profile =
      dt_ioppr_get_pipe_current_profile_info(module, piece->pipe);
    dt_color_picker_helper(dsc, pixel, roi, box,
                           darktable.lib->proxy.colorpicker.primary_sample->denoise,
                           pick, image_cst,
                           dt_iop_color_picker_get_active_cst(module), profile);
  }

  for_four_channels(k)
  {
    picked_color_min[k] = pick[DT_PICK_MIN][k];
    picked_color_max[k] = pick[DT_PICK_MAX][k];
    picked_color[k] = pick[DT_PICK_MEAN][k];
  }
}


#ifdef HAVE_OPENCL
// helper for OpenCL color picking for module
//
// this algorithm is inefficient as hell when it comes to larger
// images. it's only acceptable as long as we work on small image
// sizes like in image preview an OpenCL picker implementation would
// help
//
// FIXME: make called with: lib_colorpicker_sample_statistics pick
static void _pixelpipe_picker_cl(const int devid,
                                 dt_iop_module_t *module,
                                 dt_dev_pixelpipe_iop_t *piece,
                                 dt_iop_buffer_dsc_t *dsc,
                                 cl_mem img,
                                 const dt_iop_roi_t *roi,
                                 float *picked_color,
                                 float *picked_color_min,
                                 float *picked_color_max,
                                 float *buffer,
                                 const size_t bufsize,
                                 const dt_iop_colorspace_type_t image_cst,
                                 const dt_pixelpipe_picker_source_t picker_source)
{
  int box[4] = { 0 };

  if(_pixelpipe_picker_box(module, roi,
                           darktable.lib->proxy.colorpicker.primary_sample,
                           picker_source, box))
  {
    for_four_channels(k)
    {
      picked_color_min[k] = FLT_MAX;
      picked_color_max[k] = -FLT_MAX;
      picked_color[k] = 0.0f;
    }

    return;
  }

  const size_t origin[3] = { box[0], box[1], 0 };
  const size_t region[3] = { box[2] - box[0], box[3] - box[1], 1 };

  float *pixel = NULL;
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
  const cl_int err = dt_opencl_read_host_from_device_raw(devid, pixel, img,
                                                         origin, region, region[0] * bpp,
                                                         CL_TRUE);

  if(err != CL_SUCCESS) goto error;

  dt_iop_roi_t roi_copy = (dt_iop_roi_t)
    {.x      = roi->x + box[0],
     .y      = roi->y + box[1],
     .width  = region[0],
     .height = region[1] };

  box[0] = 0;
  box[1] = 0;
  box[2] = region[0];
  box[3] = region[1];

  // FIXME: don't need to initialize this if dt_color_picker_helper() does
  lib_colorpicker_stats pick =
    { { 0.0f, 0.0f, 0.0f, 0.0f },
      { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX },
      { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX } };

  const dt_iop_order_iccprofile_info_t *const profile =
    dt_ioppr_get_pipe_current_profile_info(module, piece->pipe);

  dt_color_picker_helper(dsc, pixel, &roi_copy, box,
                         darktable.lib->proxy.colorpicker.primary_sample->denoise,
                         pick, image_cst,
                         dt_iop_color_picker_get_active_cst(module), profile);

  for_four_channels(k)
  {
    picked_color_min[k] = pick[DT_PICK_MIN][k];
    picked_color_max[k] = pick[DT_PICK_MAX][k];
    picked_color[k] = pick[DT_PICK_MEAN][k];
  }

error:
  dt_free_align(tmpbuf);
}
#endif

static void _pixelpipe_pick_samples(dt_develop_t *dev,
                                    dt_iop_module_t *module,
                                    const dt_iop_buffer_dsc_t *dsc,
                                    const float *const input,
                                    const dt_iop_roi_t *roi_in)
{
  const dt_iop_order_iccprofile_info_t *const histogram_profile =
    dt_ioppr_get_histogram_profile_info(dev);
  const dt_iop_order_iccprofile_info_t *const display_profile
    = dt_ioppr_add_profile_info_to_list(dev, darktable.color_profiles->display_type,
                                        darktable.color_profiles->display_filename,
                                        INTENT_RELATIVE_COLORIMETRIC);

  // if we have a primary picker, prepend to the list of any live
  // samples, so that we don't have to differentiate when looping
  // through the pixels
  GSList *samples = darktable.lib->proxy.colorpicker.live_samples;
  GSList primary;
  if(darktable.lib->proxy.colorpicker.picker_proxy)
  {
    primary.data = darktable.lib->proxy.colorpicker.primary_sample;
    primary.next = samples;
    samples = &primary;
  }

  while(samples)
  {
    int box[4];
    dt_colorpicker_sample_t *sample = samples->data;
    if(!sample->locked &&
       !_pixelpipe_picker_box(module, roi_in, sample, PIXELPIPE_PICKER_INPUT, box))
    {
      // pixel input is in display profile, hence the sample output will be as well
      dt_color_picker_helper(dsc, input, roi_in, box, sample->denoise,
                             sample->display,
                             IOP_CS_RGB, IOP_CS_RGB, display_profile);

      // NOTE: conversions assume that dt_aligned_pixel_t[x] has no
      // padding, e.g. is equivalent to float[x*4], and that on failure
      // it's OK not to touch output
      int converted_cst;
      dt_ioppr_transform_image_colorspace(module, sample->display[0], sample->lab[0],
                                          3, 1, IOP_CS_RGB, IOP_CS_LAB,
                                          &converted_cst, display_profile);
      if(display_profile && histogram_profile)
        dt_ioppr_transform_image_colorspace_rgb
          (sample->display[0], sample->scope[0], 3, 1,
           display_profile, histogram_profile, "primary picker");
    }
    samples = g_slist_next(samples);
  }
}

// returns TRUE if blend process need the module default colorspace
static gboolean _transform_for_blend(const dt_iop_module_t *const self,
                                     const dt_dev_pixelpipe_iop_t *const piece)
{
  const dt_develop_blend_params_t *const d =
    (const dt_develop_blend_params_t *)piece->blendop_data;
  if(d)
  {
    // check only if blend is active
    if((self->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
       && (d->mask_mode != DEVELOP_MASK_DISABLED))
    {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean _request_color_pick(dt_dev_pixelpipe_t *pipe,
                                    dt_develop_t *dev,
                                    dt_iop_module_t *module)
{
  // Does the current active module need a picker?
  return
    // pick from preview pipe to get pixels outside the viewport
    dev->gui_attached && pipe == dev->preview_pipe
    // there is an active picker widget
    && darktable.lib->proxy.colorpicker.picker_proxy
    // only modules with focus can pick
    && module == dev->gui_module
    // and they are enabled
    && dev->gui_module->enabled
    // and they want to pick ;)
    && module->request_color_pick != DT_REQUEST_COLORPICK_OFF;
}

static void _collect_histogram_on_CPU(dt_dev_pixelpipe_t *pipe,
                                      dt_develop_t *dev,
                                      float *input,
                                      const dt_iop_roi_t *roi_in,
                                      dt_iop_module_t *module,
                                      dt_dev_pixelpipe_iop_t *piece,
                                      dt_pixelpipe_flow_t *pixelpipe_flow)
{
  // histogram collection for module
  if((dev->gui_attached || !(piece->request_histogram & DT_REQUEST_ONLY_IN_GUI))
     && (piece->request_histogram & DT_REQUEST_ON))
  {
    _histogram_collect(piece, input, roi_in, &(piece->histogram), piece->histogram_max);
    *pixelpipe_flow |= (PIXELPIPE_FLOW_HISTOGRAM_ON_CPU);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_HISTOGRAM_NONE | PIXELPIPE_FLOW_HISTOGRAM_ON_GPU);

    if(piece->histogram
       && (module->request_histogram & DT_REQUEST_ON)
       && (pipe->type & DT_DEV_PIXELPIPE_PREVIEW))
    {
      const size_t buf_size = 4 * piece->histogram_stats.bins_count * sizeof(uint32_t);
      module->histogram = realloc(module->histogram, buf_size);
      memcpy(module->histogram, piece->histogram, buf_size);
      module->histogram_stats = piece->histogram_stats;
      memcpy(module->histogram_max, piece->histogram_max, sizeof(piece->histogram_max));
      if(module->widget)
        dt_control_queue_redraw_widget(module->widget);
    }
  }
}

static gboolean _pixelpipe_process_on_CPU(
                 dt_dev_pixelpipe_t *pipe,
                 dt_develop_t *dev,
                 float *input,
                 dt_iop_buffer_dsc_t *input_format,
                 const dt_iop_roi_t *roi_in,
                 void **output,
                 dt_iop_buffer_dsc_t **out_format,
                 const dt_iop_roi_t *roi_out,
                 dt_iop_module_t *module,
                 dt_dev_pixelpipe_iop_t *piece,
                 dt_develop_tiling_t *tiling,
                 dt_pixelpipe_flow_t *pixelpipe_flow)
{
  if(dt_atomic_get_int(&pipe->shutdown))
    return TRUE;

  // the data buffers must always have a 64 alignment
  if((((uintptr_t)input) & 63) || (((uintptr_t)*output) & 63))
    dt_print(DT_DEBUG_ALWAYS,
             "[pixelpipe_process_on_CPU] buffer aligment problem: IN=%p OUT=%p\n",
             input, *output);

  // Fetch RGB working profile
  // if input is RAW, we can't color convert because RAW is not in a color space
  // so we send NULL to by-pass
  const dt_iop_order_iccprofile_info_t *const work_profile =
    (input_format->cst != IOP_CS_RAW)
      ? dt_ioppr_get_pipe_work_profile_info(pipe)
      : NULL;

  const int cst_from = input_format->cst;
  const int cst_to = module->input_colorspace(module, pipe, piece);

  if(darktable.unmuted & DT_DEBUG_PIPE)
  {
    const gboolean no_conversion = (cst_from == cst_to) || (work_profile == NULL)
                    || (work_profile->type == DT_COLORSPACE_NONE);
    if(!no_conversion)
    {
      char profiles[128] = { 0 };
      snprintf(profiles, sizeof(profiles), "%s -> %s\n",
               dt_iop_colorspace_to_name(cst_from),
               dt_iop_colorspace_to_name(cst_to));

      dt_print_pipe(DT_DEBUG_PIPE,
                    "transform colorspace CPU",
                    piece->pipe, module, roi_in, roi_out, profiles);

    }
  }

  // transform to module input colorspace
  dt_ioppr_transform_image_colorspace
    (module, input, input, roi_in->width, roi_in->height, cst_from,
     cst_to, &input_format->cst, work_profile);

  if(dt_atomic_get_int(&pipe->shutdown))
    return TRUE;

  _collect_histogram_on_CPU(pipe, dev, input, roi_in, module, piece, pixelpipe_flow);

  if(dt_atomic_get_int(&pipe->shutdown))
    return TRUE;

  const size_t in_bpp = dt_iop_buffer_dsc_to_bpp(input_format);
  const size_t bpp = dt_iop_buffer_dsc_to_bpp(*out_format);

  const gboolean fitting = dt_tiling_piece_fits_host_memory
    (MAX(roi_in->width, roi_out->width),
     MAX(roi_in->height, roi_out->height),
     MAX(in_bpp, bpp),
     tiling->factor, tiling->overhead);

  /* process module on cpu. use tiling if needed and possible. */

  const gboolean pfm_dump = darktable.dump_pfm_pipe
    && (piece->pipe->type & (DT_DEV_PIXELPIPE_FULL | DT_DEV_PIXELPIPE_EXPORT));

  if(pfm_dump)
    dt_dump_pipe_pfm(module->so->op, input,
                     roi_in->width, roi_in->height, in_bpp,
                     TRUE, dt_dev_pixelpipe_type_to_str(piece->pipe->type));

  if(!fitting && piece->process_tiling_ready)
  {
    dt_print_pipe(DT_DEBUG_PIPE,
                  "process TILE", piece->pipe, module, roi_in, roi_out, "\n");
    module->process_tiling(module, piece, input, *output, roi_in, roi_out, in_bpp);

    *pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU
                        | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU);
  }
  else
  {
    dt_print_pipe(DT_DEBUG_PIPE,
       "pixelpipe_process_on_CPU",
       piece->pipe, module, roi_in, roi_out,
       (fitting)
       ? "\n"
       : "Warning: processed without tiling even if memory requirements are not met\n");

    // this code section is for simplistic benchmarking via --bench-module
    if((piece->pipe->type & (DT_DEV_PIXELPIPE_FULL | DT_DEV_PIXELPIPE_EXPORT))
       && darktable.bench_module)
    {
      if(dt_str_commasubstring(darktable.bench_module, module->so->op))
      {
        dt_times_t start;
        dt_times_t end;
        const int old_muted = darktable.unmuted;
        darktable.unmuted = 0;
        const gboolean full = piece->pipe->type & DT_DEV_PIXELPIPE_FULL;
        const int counter = (piece->pipe->type & DT_DEV_PIXELPIPE_FULL) ? 100 : 50;
        const float mpix = (roi_out->width * roi_out->height) / 1.0e6;

        if(module->process_plain)
        {
          dt_get_times(&start);
          for(int i = 0; i < counter; i++)
            module->process_plain(module, piece, input, *output, roi_in, roi_out);
          dt_get_times(&end);
          const float clock = (end.clock - start.clock) / (float) counter;
          dt_print(DT_DEBUG_ALWAYS,
                   "[bench module plain] [%s] `%15s' takes %8.5fs,%7.2fmpix,%9.3fpix/us\n",
                   full ? "full" : "export", module->so->op, clock, mpix, mpix/clock);
        }
        darktable.unmuted = old_muted;
      }
    }
    module->process(module, piece, input, *output, roi_in, roi_out);

    *pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU
                         | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
  }

  if(pfm_dump)
  {
    dt_dump_pipe_pfm(module->so->op, *output,
                     roi_out->width, roi_out->height, bpp,
                     FALSE, dt_dev_pixelpipe_type_to_str(piece->pipe->type));
    _dump_pipe_pfm_diff(module->so->op, input, roi_in, in_bpp, *output, roi_out, bpp,
                        dt_dev_pixelpipe_type_to_str(piece->pipe->type));
  }

  // and save the output colorspace
  pipe->dsc.cst = module->output_colorspace(module, pipe, piece);

  if(dt_atomic_get_int(&pipe->shutdown))
    return TRUE;

  // color picking for module
  if(_request_color_pick(pipe, dev, module))
  {
    _pixelpipe_picker(module, piece, &piece->dsc_in, (float *)input, roi_in,
                      module->picked_color,
                      module->picked_color_min,
                      module->picked_color_max,
                      input_format->cst, PIXELPIPE_PICKER_INPUT);

    _pixelpipe_picker(module, piece, &pipe->dsc, (float *)(*output), roi_out,
                      module->picked_output_color,
                      module->picked_output_color_min,
                      module->picked_output_color_max,
                      pipe->dsc.cst, PIXELPIPE_PICKER_OUTPUT);

    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals,
                                  DT_SIGNAL_CONTROL_PICKERDATA_READY, module, piece);
  }

  if(dt_atomic_get_int(&pipe->shutdown))
    return TRUE;

  // blend needs input/output images with default colorspace
  if(_transform_for_blend(module, piece))
  {
    const dt_iop_colorspace_type_t blend_cst =
      dt_develop_blend_colorspace(piece, pipe->dsc.cst);
    dt_ioppr_transform_image_colorspace(module, input, input, roi_in->width, roi_in->height,
                                        input_format->cst, blend_cst, &input_format->cst,
                                        work_profile);
    dt_ioppr_transform_image_colorspace(module, *output, *output,
                                        roi_out->width, roi_out->height,
                                        pipe->dsc.cst, blend_cst, &pipe->dsc.cst,
                                        work_profile);
  }

  if(dt_atomic_get_int(&pipe->shutdown))
    return TRUE;

  /* process blending on CPU */
  dt_develop_blend_process(module, piece, input, *output, roi_in, roi_out);
  *pixelpipe_flow |= (PIXELPIPE_FLOW_BLENDED_ON_CPU);
  *pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_GPU);

  if(dt_atomic_get_int(&pipe->shutdown))
    return TRUE;
  else
    return FALSE;
}

static inline gboolean _check_good_pipe(dt_dev_pixelpipe_t *pipe)
{
  return (pipe->type & (DT_DEV_PIXELPIPE_FULL | DT_DEV_PIXELPIPE_PREVIEW))
         && (pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_NONE);
}

static inline gboolean _check_module_next_important(dt_dev_pixelpipe_t *pipe,
                                                    dt_iop_module_t *module)
{
  if(!_check_good_pipe(pipe)) return FALSE;
  return ((module->flags() & IOP_FLAGS_CACHE_IMPORTANT_NEXT)
          || module->cache_next_important);
}

static inline gboolean _check_module_now_important(dt_dev_pixelpipe_t *pipe,
                                                   dt_iop_module_t *module)
{
  if(!_check_good_pipe(pipe)) return FALSE;
  return (module->flags() & IOP_FLAGS_CACHE_IMPORTANT_NOW);
}

#ifdef HAVE_OPENCL
static inline gboolean _opencl_pipe_isok(dt_dev_pixelpipe_t *pipe)
{
  return darktable.opencl->inited
         && !darktable.opencl->stopped
         && pipe->opencl_enabled
         && (pipe->devid >= 0);
}
#endif

// recursive helper for process, returns TRUE in case of unfinished work or error
static gboolean _dev_pixelpipe_process_rec(
                 dt_dev_pixelpipe_t *pipe,
                 dt_develop_t *dev,
                 void **output,
                 void **cl_mem_output,
                 dt_iop_buffer_dsc_t **out_format,
                 const dt_iop_roi_t *roi_out,
                 GList *modules,
                 GList *pieces,
                 const int pos)
{
  if(dt_atomic_get_int(&pipe->shutdown))
    return TRUE;

  dt_iop_roi_t roi_in = *roi_out;

  char module_name[256] = { 0 };
  void *input = NULL;
  void *cl_mem_input = NULL;
  *cl_mem_output = NULL;
  dt_iop_module_t *module = NULL;
  dt_dev_pixelpipe_iop_t *piece = NULL;

  // if a module is active, check if this module allow a fast pipe run
  if(darktable.develop && dev->gui_module
     && dev->gui_module->flags() & IOP_FLAGS_ALLOW_FAST_PIPE)
    pipe->type |= DT_DEV_PIXELPIPE_FAST;
  else
    pipe->type &= ~DT_DEV_PIXELPIPE_FAST;

  if(modules)
  {
    module = (dt_iop_module_t *)modules->data;
    piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
    // skip this module?
    if(!piece->enabled
       || (dev->gui_module && dev->gui_module != module
           && dev->gui_module->operation_tags_filter() & module->operation_tags()))
      return _dev_pixelpipe_process_rec(pipe, dev, output, cl_mem_output, out_format,
                                        &roi_in,
                                        g_list_previous(modules),
                                        g_list_previous(pieces), pos - 1);
  }

  if(module)
    g_strlcpy(module_name, module->op, MIN(sizeof(module_name), sizeof(module->op)));
  get_output_format(module, pipe, piece, dev, *out_format);
  const size_t bpp = dt_iop_buffer_dsc_to_bpp(*out_format);
  const size_t bufsize = (size_t)bpp * roi_out->width * roi_out->height;

  // 1) if cached buffer is still available, return data
  if(dt_atomic_get_int(&pipe->shutdown))
    return TRUE;

  gboolean cache_available = FALSE;
  uint64_t basichash = 0;
  uint64_t hash = 0;

  // we do not want data from the preview pixelpipe cache
  // for gamma so we can compute the final scope
  const gboolean gamma_preview =
    (pipe->type & DT_DEV_PIXELPIPE_PREVIEW)
    && (module != NULL)
    && dt_iop_module_is(module->so, "gamma");

  // we also never want any cached data is in masking mode
  if(!gamma_preview
      && (pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_NONE)
      && !pipe->nocache)
  {
    dt_dev_pixelpipe_cache_fullhash(pipe->image.id, roi_out, pipe, pos, &basichash, &hash);
    // dt_dev_pixelpipe_cache_available() tests for masking mode and returns FALSE in that case
    cache_available = dt_dev_pixelpipe_cache_available(pipe, hash, bufsize);
  }
  if(cache_available)
  {
    dt_dev_pixelpipe_cache_get(pipe, basichash, hash, bufsize,
                               output, out_format, module, FALSE);

    if(dt_atomic_get_int(&pipe->shutdown))
      return TRUE;

    dt_print_pipe(DT_DEBUG_PIPE,
                  "pixelpipe data: from cache", pipe, module, &roi_in, roi_out, "\n");
    // we're done! as colorpicker/scopes only work on gamma iop
    // input -- which is unavailable via cache -- there's no need to
    // run these
    return FALSE;
  }

  // 2) if history changed or exit event, abort processing?
  // preview pipe: abort on all but zoom events (same buffer anyways)
  if(dt_iop_breakpoint(dev, pipe)) return TRUE;
  // if image has changed, stop now.
  if(pipe == dev->pipe && dev->image_force_reload) return TRUE;
  if(pipe == dev->preview_pipe && dev->preview_loading) return TRUE;
  if(pipe == dev->preview2_pipe && dev->preview2_loading) return TRUE;
  if(dev->gui_leaving) return TRUE;

  // 3) input -> output
  if(!modules)
  {
    // 3a) import input array with given scale and roi
    if(dt_atomic_get_int(&pipe->shutdown))
      return TRUE;

    dt_times_t start;
    dt_get_perf_times(&start);
    // we're looking for the full buffer
    if(roi_out->scale == 1.0f
       && roi_out->x == 0 && roi_out->y == 0
       && pipe->iwidth == roi_out->width
       && pipe->iheight == roi_out->height)
    {
      *output = pipe->input;
      dt_print_pipe(DT_DEBUG_PIPE,
                    "pixelpipe data: full", pipe, module, &roi_in, roi_out, "\n");
    }
    else if(dt_dev_pixelpipe_cache_get(pipe, basichash, hash, bufsize,
                                       output, out_format, NULL, FALSE))
    {
      if(roi_in.scale == 1.0f)
      {
        // fast branch for 1:1 pixel copies.

        // last minute clamping to catch potential out-of-bounds in roi_in and roi_out
        const int in_x = MAX(roi_in.x, 0);
        const int in_y = MAX(roi_in.y, 0);
        const int cp_width = MAX(0, MIN(roi_out->width, pipe->iwidth - in_x));
        const int cp_height = MIN(roi_out->height, pipe->iheight - in_y);
        dt_print_pipe(DT_DEBUG_PIPE,
          (cp_width > 0) ? "pixelpipe data: 1:1 copied" : "pixelpipe data: 1:1 none",
          pipe, module, &roi_in, roi_out, "bpp=%d\n", bpp);
        if(cp_width > 0)
        {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
          dt_omp_firstprivate(bpp, cp_height, cp_width, in_x, in_y) \
          shared(pipe, roi_out, roi_in, output) \
          schedule(static)
#endif
          for(int j = 0; j < cp_height; j++)
            memcpy(((char *)*output) + (size_t)bpp * j * roi_out->width,
                   ((char *)pipe->input) + (size_t)bpp * (in_x + (in_y + j) * pipe->iwidth),
                   (size_t)bpp * cp_width);
        }
      }
      else
      {
        roi_in.x /= roi_out->scale;
        roi_in.y /= roi_out->scale;
        roi_in.width = pipe->iwidth;
        roi_in.height = pipe->iheight;
        roi_in.scale = 1.0f;
        dt_print_pipe(DT_DEBUG_PIPE,
          "pixelpipe data: clip&zoom", pipe, module, &roi_in, roi_out, "\n");
        dt_iop_clip_and_zoom(*output, pipe->input, roi_out, &roi_in,
                             roi_out->width, pipe->iwidth);
      }
    }

    dt_show_times_f(&start, "[dev_pixelpipe]",
                    "initing base buffer [%s]", dt_dev_pixelpipe_type_to_str(pipe->type));

    if(dt_atomic_get_int(&pipe->shutdown))
      return TRUE;

    return FALSE;
  }

  // 3b) recurse and obtain output array in &input

  // get region of interest which is needed in input
  if(dt_atomic_get_int(&pipe->shutdown))
    return TRUE;

  module->modify_roi_in(module, piece, roi_out, &roi_in);
  if((darktable.unmuted & DT_DEBUG_PIPE) && memcmp(roi_out, &roi_in, sizeof(dt_iop_roi_t)))
    dt_print_pipe(DT_DEBUG_PIPE,
                  "modify roi IN", piece->pipe, module, &roi_in, roi_out, "\n");
  // recurse to get actual data of input buffer

  dt_iop_buffer_dsc_t _input_format = { 0 };
  dt_iop_buffer_dsc_t *input_format = &_input_format;

  piece = (dt_dev_pixelpipe_iop_t *)pieces->data;

  piece->processed_roi_in = roi_in;
  piece->processed_roi_out = *roi_out;

  if(_dev_pixelpipe_process_rec(pipe, dev, &input, &cl_mem_input, &input_format, &roi_in,
                                g_list_previous(modules),
                                g_list_previous(pieces), pos - 1))
    return TRUE;

  const size_t in_bpp = dt_iop_buffer_dsc_to_bpp(input_format);

  piece->dsc_out = piece->dsc_in = *input_format;

  module->output_format(module, pipe, piece, &piece->dsc_out);

  **out_format = pipe->dsc = piece->dsc_out;

  const size_t out_bpp = dt_iop_buffer_dsc_to_bpp(*out_format);

  // reserve new cache line: output
  if(dt_atomic_get_int(&pipe->shutdown))
    return TRUE;

  gboolean important = FALSE;

  if((pipe->type & DT_DEV_PIXELPIPE_PREVIEW) && module)
    important = (dt_iop_module_is(module->so, "colorout"));

  if((pipe->type & DT_DEV_PIXELPIPE_FULL)
      && module
      && (pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_NONE))
  {
    important = (dt_iop_module_is(module->so, "gamma"));
    important |= _check_module_now_important(pipe, module);
    // we might have a hint from the previous module for being important
    important |= pipe->next_important_module;
  }
  pipe->next_important_module = FALSE;
  dt_dev_pixelpipe_cache_get(pipe, basichash, hash, bufsize,
                             output, out_format, module, important);

  if(dt_atomic_get_int(&pipe->shutdown))
    return TRUE;

  dt_times_t start;
  dt_get_perf_times(&start);

  dt_pixelpipe_flow_t pixelpipe_flow =
    (PIXELPIPE_FLOW_NONE | PIXELPIPE_FLOW_HISTOGRAM_NONE);

  // special case: user requests to see channel data in the parametric
  // mask of a module, or the blending mask. In that case we skip all
  // modules manipulating pixel content and only process image
  // distorting modules. Finally "gamma" is responsible for displaying
  // channel/mask data accordingly.
  // FIXME: Could we do a copy by roi here ?
  if(!dt_iop_module_is(module->so, "gamma")
     && (pipe->mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE)
     && !(module->operation_tags() & IOP_TAG_DISTORT)
     && (in_bpp == out_bpp)
     && !memcmp(&roi_in, roi_out, sizeof(struct dt_iop_roi_t)))
  {
    dt_print_pipe(DT_DEBUG_PIPE,
                    "pixelpipe bypass", pipe, module, &roi_in, roi_out, "\n");
    // since we're not actually running the module, the output format
    // is the same as the input format
    **out_format = pipe->dsc = piece->dsc_out = piece->dsc_in;

#ifdef HAVE_OPENCL
    if(_opencl_pipe_isok(pipe) && (cl_mem_input != NULL))
      *cl_mem_output = cl_mem_input;
    else
#endif

    dt_iop_image_copy_by_size(*output, input,
                              roi_out->width, roi_out->height, bpp / sizeof(float));

    return FALSE;
  }


  /* get tiling requirement of module */
  dt_develop_tiling_t tiling = { 0 };
  // set sentinel value to detect whether callback set sizes
  tiling.factor_cl = tiling.maxbuf_cl = -1;
  module->tiling_callback(module, piece, &roi_in, roi_out, &tiling);
  // default to CPU size if callback didn't set GPU
  if(tiling.factor_cl < 0) tiling.factor_cl = tiling.factor;
  if(tiling.maxbuf_cl < 0) tiling.maxbuf_cl = tiling.maxbuf;

  /* does this module involve blending? */
  if(piece->blendop_data
     && ((dt_develop_blend_params_t *)piece->blendop_data)->mask_mode
     != DEVELOP_MASK_DISABLED)
  {
    /* get specific memory requirement for blending */
    dt_develop_tiling_t tiling_blendop = { 0 };
    tiling_callback_blendop(module, piece, &roi_in, roi_out, &tiling_blendop);

    /* aggregate in structure tiling */
    tiling.factor = fmax(tiling.factor, tiling_blendop.factor);
    tiling.factor_cl = fmax(tiling.factor_cl, tiling_blendop.factor);
    tiling.maxbuf = fmax(tiling.maxbuf, tiling_blendop.maxbuf);
    tiling.maxbuf_cl = fmax(tiling.maxbuf_cl, tiling_blendop.maxbuf);
    tiling.overhead = fmax(tiling.overhead, tiling_blendop.overhead);
  }

  /* remark: we do not do tiling for blendop step, neither in opencl
     nor on cpu. if overall tiling requirements (maximum of module and
     blendop) require tiling for opencl path, then following blend
     step is anyhow done on cpu. we assume that blending itself will
     never require tiling in cpu path, because memory requirements
     will still be low enough. */

  assert(tiling.factor > 0.0f);
  assert(tiling.factor_cl > 0.0f);

  if(dt_atomic_get_int(&pipe->shutdown))
    return TRUE;

#ifdef HAVE_OPENCL

  // Fetch RGB working profile
  // if input is RAW, we can't color convert because RAW is not in a color space
  // so we send NULL to by-pass
  const dt_iop_order_iccprofile_info_t *const work_profile =
    (input_format->cst != IOP_CS_RAW)
    ? dt_ioppr_get_pipe_work_profile_info(pipe)
    : NULL;

  /* do we have opencl at all? did user tell us to use it? did we get a resource? */
  if(_opencl_pipe_isok(pipe))
  {
    gboolean success_opencl = TRUE;
    dt_iop_colorspace_type_t input_cst_cl = input_format->cst;

    /* if input is on gpu memory only, remember this fact to later
     * take appropriate action */
    gboolean valid_input_on_gpu_only = (cl_mem_input != NULL);

    const float required_factor_cl = fmaxf(1.0f, (valid_input_on_gpu_only)
                                           ? tiling.factor_cl - 1.0f
                                           : tiling.factor_cl);

    /* general remark: in case of opencl errors within modules or
       out-of-memory on GPU, we transparently fall back to the
       respective cpu module and continue in pixelpipe. If we
       encounter errors we set pipe->opencl_error=TRUE, return this
       function with value 1, and leave appropriate action to the
       calling function, which normally would restart pixelpipe
       without opencl.  Late errors are sometimes detected when trying
       to get back data from device into host memory and are treated
       in the same manner. */

    /* test for a possible opencl path after checking some module
       specific pre-requisites */
    gboolean possible_cl =
      (module->process_cl && piece->process_cl_ready
       && !((pipe->type & (DT_DEV_PIXELPIPE_PREVIEW | DT_DEV_PIXELPIPE_PREVIEW2))
            && (module->flags() & IOP_FLAGS_PREVIEW_NON_OPENCL)));

    const int m_bpp = MAX(in_bpp, bpp);
    const gboolean fits_on_device =
      dt_opencl_image_fits_device(pipe->devid,
                                  MAX(roi_in.width, roi_out->width),
                                  MAX(roi_in.height, roi_out->height),
                                  m_bpp, required_factor_cl, tiling.overhead);

    if(possible_cl && !fits_on_device)
    {
      if(!piece->process_tiling_ready)
        possible_cl = FALSE;

      const float advantage = darktable.opencl->dev[pipe->devid].advantage;
      if(possible_cl && (advantage > 0.0f))
      {
        const float tilemem_cl = dt_tiling_estimate_clmem(&tiling, piece,
                                                          &roi_in, roi_out, m_bpp);
        const float tilemem_cpu = dt_tiling_estimate_cpumem(&tiling, piece,
                                                            &roi_in, roi_out, m_bpp);
        if((tilemem_cpu * advantage) < tilemem_cl)
        {
          dt_print(DT_DEBUG_OPENCL | DT_DEBUG_TILING,
                   "[dt_dev_pixelpipetiling_cl] [%s] estimates cpu"
                   " advantage in `%s', (dev=%i, adv=%.2f, GPU %.2f CPU %.2f)\n",
                   dt_dev_pixelpipe_type_to_str(pipe->type), module->op, pipe->devid,
                   advantage, tilemem_cl / 1e9, tilemem_cpu / 1e9);
          possible_cl = FALSE;
        }
      }
    }

    if(possible_cl)
    {
      if(fits_on_device)
      {
        /* image is small enough -> try to directly process entire image with opencl */
        /* input is not on gpu memory -> copy it there */
        if(cl_mem_input == NULL)
        {
          cl_mem_input = dt_opencl_alloc_device(pipe->devid,
                                                roi_in.width, roi_in.height, in_bpp);
          if(cl_mem_input == NULL)
          {
            dt_print(DT_DEBUG_OPENCL,
                     "[opencl_pixelpipe] [%s] couldn't generate input buffer"
                     " for module `%s'\n",
                     dt_dev_pixelpipe_type_to_str(pipe->type), module->op);
            success_opencl = FALSE;
          }

          if(success_opencl)
          {
            cl_int err = dt_opencl_write_host_to_device(pipe->devid, input, cl_mem_input,
                                                        roi_in.width, roi_in.height,
                                                        in_bpp);
            if(err != CL_SUCCESS)
            {
              dt_print(DT_DEBUG_OPENCL,
                       "[opencl_pixelpipe] [%s] couldn't copy image to opencl"
                       " device for module `%s'\n",
                       dt_dev_pixelpipe_type_to_str(pipe->type), module->op);
              success_opencl = FALSE;
            }
          }
        }

        if(dt_atomic_get_int(&pipe->shutdown))
        {
          dt_opencl_release_mem_object(cl_mem_input);
          return TRUE;
        }

        /* try to allocate GPU memory for output */
        if(success_opencl)
        {
          *cl_mem_output = dt_opencl_alloc_device(pipe->devid,
                                                  roi_out->width, roi_out->height, bpp);
          if(*cl_mem_output == NULL)
          {
            dt_print(DT_DEBUG_OPENCL,
                     "[opencl_pixelpipe] [%s] couldn't allocate output buffer"
                     " for module `%s'\n",
                     dt_dev_pixelpipe_type_to_str(pipe->type), module->op);
            success_opencl = FALSE;
          }
        }

        // fprintf(stderr, "[opencl_pixelpipe 2] for module `%s', have bufs %p and %p \n", module->op,
        // cl_mem_input, *cl_mem_output);

        // indirectly give gpu some air to breathe (and to do display related stuff)
        dt_iop_nap(dt_opencl_micro_nap(pipe->devid));

        // transform to input colorspace
        if(success_opencl)
        {
          const int cst_from = input_cst_cl;
          const int cst_to = module->input_colorspace(module, pipe, piece);

          if(darktable.unmuted & DT_DEBUG_PIPE)
          {
            const gboolean no_conversion = (cst_from == cst_to) || (work_profile == NULL)
                            || (work_profile->type == DT_COLORSPACE_NONE);
            if(!no_conversion)
            {
              char profiles[128] = { 0 };
              snprintf(profiles, sizeof(profiles), "%s -> %s\n",
                dt_iop_colorspace_to_name(cst_from), dt_iop_colorspace_to_name(cst_to));

              dt_print_pipe(DT_DEBUG_PIPE,
                            "transform colorspace CL", piece->pipe, module, &roi_in, roi_out, profiles);
            }
          }

          success_opencl = dt_ioppr_transform_image_colorspace_cl
            (module, piece->pipe->devid,
             cl_mem_input, cl_mem_input,
             roi_in.width, roi_in.height,
             input_cst_cl,
             module->input_colorspace(module, pipe, piece),
             &input_cst_cl,
             work_profile);
        }

        // histogram collection for module
        if(success_opencl
           && (dev->gui_attached
               || !(piece->request_histogram & DT_REQUEST_ONLY_IN_GUI))
           && (piece->request_histogram & DT_REQUEST_ON))
        {
          // we abuse the empty output buffer on host for intermediate storage of data in
          // histogram_collect_cl()
          const size_t outbufsize = bpp * roi_out->width * roi_out->height;

          _histogram_collect_cl(pipe->devid, piece, cl_mem_input,
                                &roi_in, &(piece->histogram),
                                piece->histogram_max, *output, outbufsize);
          pixelpipe_flow |= (PIXELPIPE_FLOW_HISTOGRAM_ON_GPU);
          pixelpipe_flow &= ~(PIXELPIPE_FLOW_HISTOGRAM_NONE
                              | PIXELPIPE_FLOW_HISTOGRAM_ON_CPU);

          if(piece->histogram
             && (module->request_histogram & DT_REQUEST_ON)
             && (pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW)
          {
            const size_t buf_size =
              sizeof(uint32_t) * 4 * piece->histogram_stats.bins_count;
            module->histogram = realloc(module->histogram, buf_size);
            memcpy(module->histogram, piece->histogram, buf_size);
            module->histogram_stats = piece->histogram_stats;
            memcpy(module->histogram_max, piece->histogram_max,
                   sizeof(piece->histogram_max));

            if(module->widget) dt_control_queue_redraw_widget(module->widget);
          }
        }

        if(dt_atomic_get_int(&pipe->shutdown))
          return TRUE;

        /* now call process_cl of module; module should emit
           meaningful messages in case of error */
        if(success_opencl)
        {
          dt_print_pipe(DT_DEBUG_PIPE,
                        "pixelpipe_process_CL",
                        piece->pipe, module, &roi_in, roi_out, "\n");

          // this code section is for simplistic benchmarking via --bench-module
          if((piece->pipe->type & (DT_DEV_PIXELPIPE_FULL | DT_DEV_PIXELPIPE_EXPORT))
             && darktable.bench_module)
          {
            if(dt_str_commasubstring(darktable.bench_module, module->so->op))
            {
              dt_times_t bench;
              dt_times_t end;
              const int old_muted = darktable.unmuted;
              darktable.unmuted = 0;;
              const gboolean full = piece->pipe->type & DT_DEV_PIXELPIPE_FULL;
              const float mpix = (roi_out->width * roi_out->height) / 1.0e6;
              gboolean success = TRUE;
              dt_get_times(&bench);
              for(int i = 0; i < 100; i++)
              {
                if(success)
                  success = module->process_cl(module, piece, cl_mem_input, *cl_mem_output,
                                               &roi_in, roi_out);
              }
              if(success)
              {
                dt_get_times(&end);
                const float clock = (end.clock - bench.clock) / 100.0f;
                dt_print(DT_DEBUG_ALWAYS,
                         "[bench module GPU]   [%s] `%15s'"
                         " takes %8.5fs,%7.2fmpix,%9.3fpix/us\n",
                         full ? "full" : "export",
                         module->so->op,
                         clock, mpix, mpix/clock);
              }
              else
                dt_print(DT_DEBUG_ALWAYS,
                         "[bench module GPU] [%s] `%s' finished without sucess\n",
                         full ? "full" : "export", module->so->op);
              darktable.unmuted = old_muted;
            }
          }
          const gboolean pfm_dump = darktable.dump_pfm_pipe
            && (piece->pipe->type & (DT_DEV_PIXELPIPE_FULL | DT_DEV_PIXELPIPE_EXPORT))
            && dt_str_commasubstring(darktable.dump_pfm_module, module->so->op);

          if(pfm_dump)
            dt_opencl_dump_pipe_pfm(module->so->op, pipe->devid, cl_mem_input,
                                    TRUE, dt_dev_pixelpipe_type_to_str(piece->pipe->type));

          success_opencl = module->process_cl(module, piece, cl_mem_input, *cl_mem_output,
                                              &roi_in, roi_out);
          if(success_opencl && pfm_dump)
            dt_opencl_dump_pipe_pfm(module->so->op, pipe->devid, *cl_mem_output,
                                    FALSE, dt_dev_pixelpipe_type_to_str(piece->pipe->type));

          pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_GPU);
          pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_CPU
                              | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);

          // and save the output colorspace
          pipe->dsc.cst = module->output_colorspace(module, pipe, piece);
        }

        if(dt_atomic_get_int(&pipe->shutdown))
        {
          dt_opencl_release_mem_object(cl_mem_input);
          return TRUE;
        }

        // color picking for module
        if(success_opencl && _request_color_pick(pipe, dev, module))
        {
          // we abuse the empty output buffer on host for intermediate storage of data in
          // pixelpipe_picker_cl()
          const size_t outbufsize = bpp * roi_out->width * roi_out->height;

          _pixelpipe_picker_cl(pipe->devid, module, piece, &piece->dsc_in,
                               cl_mem_input, &roi_in,
                               module->picked_color, module->picked_color_min,
                               module->picked_color_max,
                               *output, outbufsize, input_cst_cl,
                               PIXELPIPE_PICKER_INPUT);
          _pixelpipe_picker_cl(pipe->devid, module, piece, &pipe->dsc, (*cl_mem_output),
                               roi_out,
                               module->picked_output_color,
                               module->picked_output_color_min,
                               module->picked_output_color_max,
                               *output, outbufsize, pipe->dsc.cst,
                               PIXELPIPE_PICKER_OUTPUT);

          DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals,
                                        DT_SIGNAL_CONTROL_PICKERDATA_READY, module, piece);
        }

        if(dt_atomic_get_int(&pipe->shutdown))
           return TRUE;

        // blend needs input/output images with default colorspace
        if(success_opencl && _transform_for_blend(module, piece))
        {
          dt_iop_colorspace_type_t blend_cst =
            dt_develop_blend_colorspace(piece, pipe->dsc.cst);

          success_opencl = dt_ioppr_transform_image_colorspace_cl
            (module, piece->pipe->devid, cl_mem_input, cl_mem_input,
             roi_in.width, roi_in.height,
             input_cst_cl, blend_cst, &input_cst_cl, work_profile);

          success_opencl &= dt_ioppr_transform_image_colorspace_cl
            (module, piece->pipe->devid, *cl_mem_output, *cl_mem_output,
             roi_out->width, roi_out->height,
             pipe->dsc.cst, blend_cst, &pipe->dsc.cst, work_profile);
        }

        /* process blending */
        if(success_opencl)
        {
          success_opencl = dt_develop_blend_process_cl
            (module, piece, cl_mem_input, *cl_mem_output,
             &roi_in, roi_out);
          pixelpipe_flow |= (PIXELPIPE_FLOW_BLENDED_ON_GPU);
          pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_CPU);
        }

        /* synchronization point for opencl pipe */
        if(success_opencl)
          success_opencl = dt_opencl_finish_sync_pipe(pipe->devid, pipe->type);

        if(dt_atomic_get_int(&pipe->shutdown))
        {
          dt_opencl_release_mem_object(cl_mem_input);
          return TRUE;
        }
      }
      else if(piece->process_tiling_ready)
      {
        /* image is too big for direct opencl processing -> try to
         * process image via tiling */

        // fprintf(stderr, "[opencl_pixelpipe 3] module '%s' tiling with process_tiling_cl\n",
        //         module->op);

        /* we might need to copy back valid image from device to host */
        if(cl_mem_input != NULL)
        {
          /* copy back to CPU buffer, then clean unneeded buffer */
          const cl_int err = dt_opencl_copy_device_to_host(pipe->devid, input, cl_mem_input,
                                                           roi_in.width, roi_in.height,
                                                           in_bpp);
          if(err != CL_SUCCESS)
          {
            /* late opencl error */
            dt_print(DT_DEBUG_OPENCL,
                     "[opencl_pixelpipe (a)] [%s] late opencl error detected while"
                     " copying back to cpu buffer: %s\n",
                     dt_dev_pixelpipe_type_to_str(pipe->type), cl_errstr(err));
            dt_opencl_release_mem_object(cl_mem_input);
            pipe->opencl_error = TRUE;
            return TRUE;
          }
          else
            input_format->cst = input_cst_cl;
          dt_opencl_release_mem_object(cl_mem_input);
          cl_mem_input = NULL;
          valid_input_on_gpu_only = FALSE;
        }

        if(dt_atomic_get_int(&pipe->shutdown))
           return TRUE;

        // indirectly give gpu some air to breathe (and to do display related stuff)
        dt_iop_nap(dt_opencl_micro_nap(pipe->devid));

        // transform to module input colorspace
        if(success_opencl)
        {
          dt_ioppr_transform_image_colorspace
            (module, input, input, roi_in.width, roi_in.height,
             input_format->cst, module->input_colorspace(module, pipe, piece),
             &input_format->cst, work_profile);
        }

        if(dt_atomic_get_int(&pipe->shutdown))
          return TRUE;

        // histogram collection for module
        if(success_opencl)
        {
          _collect_histogram_on_CPU(pipe, dev, input, &roi_in, module, piece,
                                    &pixelpipe_flow);
        }

        if(dt_atomic_get_int(&pipe->shutdown))
          return TRUE;

        /* now call process_tiling_cl of module; module should emit
           meaningful messages in case of error */
        if(success_opencl)
        {
          success_opencl = module->process_tiling_cl(module, piece, input, *output,
                                                     &roi_in, roi_out, in_bpp);
          pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_GPU
                             | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
          pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_CPU);

          // and save the output colorspace
          pipe->dsc.cst = module->output_colorspace(module, pipe, piece);
        }

        if(dt_atomic_get_int(&pipe->shutdown))
          return TRUE;

        // color picking for module
        if(success_opencl && _request_color_pick(pipe, dev, module))
        {
          _pixelpipe_picker(module, piece, &piece->dsc_in, (float *)input,
                            &roi_in, module->picked_color,
                            module->picked_color_min,
                            module->picked_color_max,
                            input_format->cst,
                            PIXELPIPE_PICKER_INPUT);
          _pixelpipe_picker(module, piece, &pipe->dsc, (float *)(*output),
                            roi_out, module->picked_output_color,
                            module->picked_output_color_min,
                            module->picked_output_color_max,
                            pipe->dsc.cst,
                            PIXELPIPE_PICKER_OUTPUT);

          DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals,
                                        DT_SIGNAL_CONTROL_PICKERDATA_READY, module, piece);
        }

        if(dt_atomic_get_int(&pipe->shutdown))
           return TRUE;

        // blend needs input/output images with default colorspace
        if(success_opencl && _transform_for_blend(module, piece))
        {
          const dt_iop_colorspace_type_t blend_cst =
            dt_develop_blend_colorspace(piece, pipe->dsc.cst);

          dt_ioppr_transform_image_colorspace(module, input, input,
                                              roi_in.width, roi_in.height,
                                              input_format->cst, blend_cst,
                                              &input_format->cst,
                                              work_profile);
          dt_ioppr_transform_image_colorspace(module, *output, *output,
                                              roi_out->width, roi_out->height,
                                              pipe->dsc.cst, blend_cst, &pipe->dsc.cst,
                                              work_profile);
        }

        if(dt_atomic_get_int(&pipe->shutdown))
          return TRUE;

        /* do process blending on cpu (this is anyhow fast enough) */
        if(success_opencl)
        {
          dt_develop_blend_process(module, piece, input, *output, &roi_in, roi_out);
          pixelpipe_flow |= (PIXELPIPE_FLOW_BLENDED_ON_CPU);
          pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_GPU);
        }

        /* synchronization point for opencl pipe */
        if(success_opencl)
          success_opencl = dt_opencl_finish_sync_pipe(pipe->devid, pipe->type);

        if(dt_atomic_get_int(&pipe->shutdown))
          return TRUE;
      }
      else
      {
        /* image is too big for direct opencl and tiling is not
         * allowed -> no opencl processing for this module */
        success_opencl = FALSE;
      }

      if(dt_atomic_get_int(&pipe->shutdown))
      {
        dt_opencl_release_mem_object(cl_mem_input);
        return TRUE;
      }

      // Test code: simulate spurious failures:
      // if(rand() % 20 == 0) success_opencl = FALSE;

      /* finally check, if we were successful */
      if(success_opencl)
      {
        /* Nice, everything went fine */

        /* Copying device buffers back to host memory is an expensive
           operation but is required for the iop cache.  As the gpu
           memory is very restricted we can't use that for a cache.
           The iop cache hit rate is much more important for the UI
           responsiveness so we make sure relevant cache line buffers
           are kept. This is true

             a) for the currently focused iop, as that is the iop
                which is most likely to change next
             b) if there is a hint for a module doing heavy processing.
             c) only for full or preview pipe
        */
        if((module == darktable.develop->gui_module) || important)
        {
          /* write back input into cache for faster re-usal (full pipe or preview) */
          if(cl_mem_input != NULL
             && (pipe->type & (DT_DEV_PIXELPIPE_FULL | DT_DEV_PIXELPIPE_PREVIEW)))
          {
            /* copy input to host memory, so we can find it in cache */
            const cl_int err = dt_opencl_copy_device_to_host(pipe->devid, input,
                                                             cl_mem_input,
                                                             roi_in.width,
                                                             roi_in.height, in_bpp);
            if(err != CL_SUCCESS)
            {
              /* late opencl error, not likely to happen here */
              dt_print(DT_DEBUG_OPENCL,
                       "[opencl_pixelpipe (e)] [%s] late opencl error"
                       " detected while copying "
                       "back to cpu buffer: %s\n",
                       dt_dev_pixelpipe_type_to_str(pipe->type), cl_errstr(err));
              /* that's all we do here, we later make sure to invalidate cache line */
            }
            else
            {
              /* success: cache line is valid now, so we will not need
                 to invalidate it later */
              valid_input_on_gpu_only = FALSE;

              input_format->cst = input_cst_cl;
              // TODO: check if we need to wait for finished opencl
              // pipe before we release cl_mem_input
              // dt_dev_finish(pipe->devid);
            }
          }

          if(dt_atomic_get_int(&pipe->shutdown))
          {
            dt_opencl_release_mem_object(cl_mem_input);
            return TRUE;
          }
        }

        /* we can now release cl_mem_input */
        dt_opencl_release_mem_object(cl_mem_input);
        cl_mem_input = NULL;
        // we speculate on the next plug-in to possibly copy back
        // cl_mem_output to output, so we're not just yet invalidating
        // the (empty) output cache line.
      }
      else
      {
        /* Bad luck, opencl failed. Let's clean up and fall back to cpu module */
        dt_print(DT_DEBUG_OPENCL,
                 "[opencl_pixelpipe] [%s] could not run module `%s' on gpu."
                 " falling back to cpu path\n",
                 dt_dev_pixelpipe_type_to_str(pipe->type), module->op);

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
          /* copy back to host memory, then clean no longer needed opencl buffer.
             important info: in order to make this possible, opencl modules must
             not spoil their input buffer, even in case of errors. */
          const cl_int err = dt_opencl_copy_device_to_host(pipe->devid, input, cl_mem_input,
                                                           roi_in.width, roi_in.height,
                                                           in_bpp);
          if(err != CL_SUCCESS)
          {
            /* late opencl error */
            dt_print(DT_DEBUG_OPENCL,
                     "[opencl_pixelpipe (b)] [%s] late opencl error detected"
                     " while copying back to cpu buffer: %s\n",
                     dt_dev_pixelpipe_type_to_str(pipe->type), cl_errstr(err));
            dt_opencl_release_mem_object(cl_mem_input);
            pipe->opencl_error = TRUE;
            return TRUE;
          }
          else
            input_format->cst = input_cst_cl;

          /* this is a good place to release event handles as we
             anyhow need to move from gpu to cpu here */
          dt_opencl_finish(pipe->devid);
          dt_opencl_release_mem_object(cl_mem_input);
          valid_input_on_gpu_only = FALSE;
        }
        if(_pixelpipe_process_on_CPU(pipe, dev, input, input_format, &roi_in, output,
                                     out_format,
                                     roi_out, module, piece, &tiling, &pixelpipe_flow))
          return TRUE;
      }

      if(dt_atomic_get_int(&pipe->shutdown))
        return TRUE;
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
        const cl_int err = dt_opencl_copy_device_to_host(pipe->devid, input, cl_mem_input,
                                                         roi_in.width, roi_in.height,
                                                         in_bpp);
        // if(rand() % 5 == 0) err = !CL_SUCCESS; // Test code: simulate spurious failures
        if(err != CL_SUCCESS)
        {
          /* late opencl error */
          dt_print(DT_DEBUG_OPENCL,
                   "[opencl_pixelpipe (c)] [%s] late opencl error detected"
                   " while copying back to cpu buffer: %s\n",
                   dt_dev_pixelpipe_type_to_str(pipe->type), cl_errstr(err));
          dt_opencl_release_mem_object(cl_mem_input);
          pipe->opencl_error = TRUE;
          return TRUE;
        }
        else
          input_format->cst = input_cst_cl;

        /* this is a good place to release event handles as we anyhow
           need to move from gpu to cpu here */
        dt_opencl_finish(pipe->devid);
        dt_opencl_release_mem_object(cl_mem_input);
        valid_input_on_gpu_only = FALSE;
      }

      if(_pixelpipe_process_on_CPU(pipe, dev, input, input_format, &roi_in,
                                   output, out_format,
                                   roi_out, module, piece, &tiling, &pixelpipe_flow))
        return TRUE;
    }

    /* input is still only on GPU? Let's invalidate CPU input buffer then */
    if(valid_input_on_gpu_only)
      dt_dev_pixelpipe_invalidate_cacheline(pipe, input, TRUE);
  }
  else
  {
    /* opencl is not inited or not enabled or we got no
     * resource/device -> everything runs on cpu */

    if(_pixelpipe_process_on_CPU(pipe, dev, input, input_format, &roi_in,
                                 output, out_format, roi_out,
                                 module, piece, &tiling, &pixelpipe_flow))
      return TRUE;
  }
#else // HAVE_OPENCL
  if(_pixelpipe_process_on_CPU(pipe, dev, input, input_format, &roi_in,
                               output, out_format, roi_out,
                               module, piece, &tiling, &pixelpipe_flow))
    return TRUE;
#endif // HAVE_OPENCL

  if(pipe->mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE)
    dt_dev_pixelpipe_invalidate_cacheline(pipe, *output, FALSE);

  char histogram_log[32] = "";
  if(!(pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_NONE))
  {
    snprintf(histogram_log, sizeof(histogram_log), ", collected histogram on %s",
             (pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_ON_GPU
                  ? "GPU"
                  : pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_ON_CPU ? "CPU" : ""));
  }

  gchar *module_label = dt_history_item_get_name(module);
  dt_show_times_f
    (&start,
     "[dev_pixelpipe]", "[%s] processed `%s' on %s%s%s, blended on %s",
     dt_dev_pixelpipe_type_to_str(pipe->type), module_label,
     pixelpipe_flow & PIXELPIPE_FLOW_PROCESSED_ON_GPU
          ? "GPU"
          : pixelpipe_flow & PIXELPIPE_FLOW_PROCESSED_ON_CPU ? "CPU" : "",
     pixelpipe_flow & PIXELPIPE_FLOW_PROCESSED_WITH_TILING ? " with tiling" : "",
     (!(pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_NONE)
      && (piece->request_histogram & DT_REQUEST_ON))
          ? histogram_log
          : "",
     pixelpipe_flow & PIXELPIPE_FLOW_BLENDED_ON_GPU
          ? "GPU"
          : pixelpipe_flow & PIXELPIPE_FLOW_BLENDED_ON_CPU ? "CPU" : "");
  g_free(module_label);
  module_label = NULL;

  // in case we get this buffer from the cache in the future, cache some stuff:
  **out_format = piece->dsc_out = pipe->dsc;

  const gboolean needs_histo = module
     && darktable.develop->gui_attached
     && module->expanded
     && module->enabled
     && (pipe->type & (DT_DEV_PIXELPIPE_FULL | DT_DEV_PIXELPIPE_PREVIEW))
     && (module->request_histogram & DT_REQUEST_EXPANDED);

  if(needs_histo)
    dt_print_pipe(DT_DEBUG_PIPE, "internal histogram", pipe, module, NULL, NULL, "\n");

  if((pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_NONE) && !needs_histo)
  {
    if(module && module == darktable.develop->gui_module)
    {
      // give the input buffer to the currently focused plugin more weight.
      // the user is likely to change that one soon, so keep it in cache.
      dt_dev_pixelpipe_important_cacheline(pipe, input, roi_in.width * roi_in.height * in_bpp);
    }

    // we check for an important hint after processing the module as we
    // want to track a runtime hint too.
    pipe->next_important_module = _check_module_next_important(pipe, module);
  }

  if(needs_histo)
  {
    pipe->nocache = TRUE;
    dt_dev_pixelpipe_invalidate_cacheline(pipe, *output, FALSE);
  }

  // warn on NaN or infinity
#ifndef _DEBUG
  if((darktable.unmuted & DT_DEBUG_NAN)
     && !dt_iop_module_is(module->so, "gamma"))
#endif
  {
    if(dt_atomic_get_int(&pipe->shutdown))
      return TRUE;

#ifdef HAVE_OPENCL
    if(*cl_mem_output != NULL)
      dt_opencl_copy_device_to_host(pipe->devid, *output, *cl_mem_output,
                                    roi_out->width, roi_out->height, bpp);
#endif

    if((*out_format)->datatype == TYPE_FLOAT
       && (*out_format)->channels == 4)
    {
      int hasinf = 0, hasnan = 0;
      dt_aligned_pixel_t min = { FLT_MAX };
      dt_aligned_pixel_t max = { -FLT_MAX };

      for(int k = 0; k < 4 * roi_out->width * roi_out->height; k++)
      {
        if((k & 3) < 3)
        {
          const float f = ((float *)(*output))[k];
          if(dt_isnan(f))
            hasnan = 1;
          else if(dt_isinf(f))
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
        dt_print(DT_DEBUG_ALWAYS,
                 "[dev_pixelpipe] module `%s' outputs NaNs! [%s]\n", module_label,
                 dt_dev_pixelpipe_type_to_str(pipe->type));
      if(hasinf)
        dt_print(DT_DEBUG_ALWAYS,
                 "[dev_pixelpipe] module `%s' outputs non-finite floats! [%s]\n",
                 module_label,
                 dt_dev_pixelpipe_type_to_str(pipe->type));
      dt_print(DT_DEBUG_ALWAYS,
               "[dev_pixelpipe] module `%s' min: (%f; %f; %f) max: (%f; %f; %f) [%s]\n",
               module_label,
               min[0], min[1], min[2], max[0], max[1], max[2],
               dt_dev_pixelpipe_type_to_str(pipe->type));
      g_free(module_label);
    }
    else if((*out_format)->datatype == TYPE_FLOAT && (*out_format)->channels == 1)
    {
      int hasinf = 0, hasnan = 0;
      float min = FLT_MAX;
      float max = -FLT_MAX;

      for(int k = 0; k < roi_out->width * roi_out->height; k++)
      {
        const float f = ((float *)(*output))[k];
        if(dt_isnan(f))
          hasnan = 1;
        else if(dt_isinf(f))
          hasinf = 1;
        else
        {
          min = fmin(f, min);
          max = fmax(f, max);
        }
      }
      module_label = dt_history_item_get_name(module);

      if(hasnan)
        dt_print(DT_DEBUG_ALWAYS,
                 "[dev_pixelpipe] module `%s' outputs NaNs! [%s]\n", module_label,
                 dt_dev_pixelpipe_type_to_str(pipe->type));
      if(hasinf)
        dt_print(DT_DEBUG_ALWAYS,
                 "[dev_pixelpipe] module `%s' outputs non-finite floats! [%s]\n",
                 module_label,
                 dt_dev_pixelpipe_type_to_str(pipe->type));
      dt_print(DT_DEBUG_ALWAYS,
               "[dev_pixelpipe] module `%s' min: (%f) max: (%f) [%s]\n",
               module_label, min, max,
               dt_dev_pixelpipe_type_to_str(pipe->type));
      g_free(module_label);
    }
  }

  // 4) colorpicker and scopes:
  if(dt_atomic_get_int(&pipe->shutdown))
    return TRUE;

  if(dev->gui_attached && !dev->gui_leaving
     && pipe == dev->preview_pipe
     && (dt_iop_module_is(module->so, "gamma"))) // only gamma provides meaningful RGB data
  {
    // Pick RGB/Lab for the primary colorpicker and live samples
    if(darktable.lib->proxy.colorpicker.picker_proxy
       || darktable.lib->proxy.colorpicker.live_samples)
      _pixelpipe_pick_samples(dev, module, *out_format,
                              (const float *const )input, &roi_in);

    // FIXME: read this from dt_ioppr_get_pipe_output_profile_info()?
    const dt_iop_order_iccprofile_info_t *const display_profile
      = dt_ioppr_add_profile_info_to_list(dev, darktable.color_profiles->display_type,
                                          darktable.color_profiles->display_filename,
                                          INTENT_RELATIVE_COLORIMETRIC);

    // Since histogram is being treated as the second-to-last link
    // in the pixelpipe and has a "process" call, why not treat it
    // as an iop? Granted, other views such as tether may also
    // benefit via a histogram.
    darktable.lib->proxy.histogram.process(darktable.lib->proxy.histogram.module, input,
                                           roi_in.width, roi_in.height,
                                           display_profile,
                                           dt_ioppr_get_histogram_profile_info(dev));
  }

  if(dt_atomic_get_int(&pipe->shutdown))
    return TRUE;

  return FALSE;
}


gboolean dt_dev_pixelpipe_process_no_gamma(
           dt_dev_pixelpipe_t *pipe,
           dt_develop_t *dev,
           const int x,
           const int y,
           const int width,
           const int height,
           const float scale)
{
  // temporarily disable gamma mapping.
  GList *gammap = g_list_last(pipe->nodes);
  dt_dev_pixelpipe_iop_t *gamma = (dt_dev_pixelpipe_iop_t *)gammap->data;

  while(!dt_iop_module_is(gamma->module->so, "gamma"))
  {
    gamma = NULL;
    gammap = g_list_previous(gammap);
    if(!gammap) break;
    gamma = (dt_dev_pixelpipe_iop_t *)gammap->data;
  }

  if(gamma) gamma->enabled = FALSE;
  const gboolean ret = dt_dev_pixelpipe_process(pipe, dev, x, y, width, height, scale);
  if(gamma) gamma->enabled = TRUE;
  return ret;
}

void dt_dev_pixelpipe_disable_after(dt_dev_pixelpipe_t *pipe, const char *op)
{
  GList *nodes = g_list_last(pipe->nodes);
  dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
  while(!dt_iop_module_is(piece->module->so, op))
  {
    piece->enabled = FALSE;
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
  while(!dt_iop_module_is(piece->module->so, op))
  {
    piece->enabled = FALSE;
    piece = NULL;
    nodes = g_list_next(nodes);
    if(!nodes) break;
    piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
  }
}

// returns TRUE in case of error or early exit
static gboolean _dev_pixelpipe_process_rec_and_backcopy(
                  dt_dev_pixelpipe_t *pipe,
                  dt_develop_t *dev,
                  void **output,
                  void **cl_mem_output,
                  dt_iop_buffer_dsc_t **out_format,
                  const dt_iop_roi_t *roi_out,
                  GList *modules,
                  GList *pieces,
                  const int pos)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex);
  darktable.dtresources.group = 4 * darktable.dtresources.level;
#ifdef HAVE_OPENCL
  dt_opencl_check_tuning(pipe->devid);
#endif
  pipe->next_important_module = FALSE;
  gboolean ret = _dev_pixelpipe_process_rec(pipe, dev, output,
                                            cl_mem_output, out_format, roi_out,
                                            modules, pieces, pos);
#ifdef HAVE_OPENCL
  // copy back final opencl buffer (if any) to CPU
  if(ret)
  {
    dt_opencl_release_mem_object(*cl_mem_output);
    *cl_mem_output = NULL;
  }
  else
  {
    if(*cl_mem_output != NULL)
    {
      cl_int err = dt_opencl_copy_device_to_host(
                    pipe->devid, *output, *cl_mem_output,
                    roi_out->width, roi_out->height,
                    dt_iop_buffer_dsc_to_bpp(*out_format));
      dt_opencl_release_mem_object(*cl_mem_output);
      *cl_mem_output = NULL;

      if(err != CL_SUCCESS)
      {
        /* this indicates a opencl problem earlier in the pipeline */
        dt_print(DT_DEBUG_OPENCL,
                 "[_dev_pixelpipe_process_rec_and_backcopy] [%s]"
                 " late opencl error detected while copying back to cpu buffer: %s\n",
                 dt_dev_pixelpipe_type_to_str(pipe->type), cl_errstr(err));
        pipe->opencl_error = TRUE;
        ret = TRUE;
      }
    }
  }
#endif
  dt_pthread_mutex_unlock(&pipe->busy_mutex);
  return ret;
}

gboolean dt_dev_pixelpipe_process(
           dt_dev_pixelpipe_t *pipe,
           dt_develop_t *dev,
           const int x,
           const int y,
           const int width,
           const int height,
           const float scale)
{
  pipe->processing = TRUE;
  pipe->nocache = FALSE;
  pipe->opencl_enabled = dt_opencl_running();
  pipe->devid = (pipe->opencl_enabled) ? dt_opencl_lock_device(pipe->type)
                                       : -1; // try to get/lock opencl resource

  dt_dev_pixelpipe_cache_checkmem(pipe);

  dt_print(DT_DEBUG_MEMORY, "[memory] before pixelpipe process\n");
  dt_print_mem_usage();

  if(pipe->devid >= 0) dt_opencl_events_reset(pipe->devid);

  dt_iop_roi_t roi = (dt_iop_roi_t){ x, y, width, height, scale };
  pipe->final_width = width;
  pipe->final_height = height;

  // get a snapshot of mask list
  if(pipe->forms) g_list_free_full(pipe->forms, (void (*)(void *))dt_masks_free_form);
  pipe->forms = dt_masks_dup_forms_deep(dev->forms, NULL);

  //  go through list of modules from the end:
  const guint pos = g_list_length(pipe->iop);
  GList *modules = g_list_last(pipe->iop);
  GList *pieces = g_list_last(pipe->nodes);

// re-entry point: in case of late opencl errors we start all over
// again with opencl-support disabled
restart:

  // check if we should obsolete caches
  if(pipe->cache_obsolete) dt_dev_pixelpipe_cache_flush(pipe);
  pipe->cache_obsolete = FALSE;

  // mask display off as a starting point
  pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  // and blendif active
  pipe->bypass_blendif = FALSE;

  void *buf = NULL;
  void *cl_mem_out = NULL;

  dt_iop_buffer_dsc_t _out_format = { 0 };
  dt_iop_buffer_dsc_t *out_format = &_out_format;

  dt_print_pipe(DT_DEBUG_PIPE,
    (pipe->devid >= 0) ? "pixelpipe starting CL" : "pixelpipe starting on CPU",
    pipe, NULL, &roi, &roi, "\n");

  // run pixelpipe recursively and get error status
  const gboolean err = _dev_pixelpipe_process_rec_and_backcopy(pipe, dev, &buf,
                                                               &cl_mem_out, &out_format,
                                                               &roi,
                                                               modules, pieces, pos);

  // get status summary of opencl queue by checking the eventlist
  const gboolean oclerr = (pipe->devid >= 0)
                          ? (dt_opencl_events_flush(pipe->devid, TRUE) != 0)
                          : FALSE;

  // Check if we had opencl errors ....  remark: opencl errors can
  // come in two ways: pipe->opencl_error is TRUE (and err is TRUE) OR
  // oclerr is TRUE

  if(oclerr || (err && pipe->opencl_error))
  {
    // Well, there were errors -> we might need to free an invalid opencl memory object
    dt_opencl_release_mem_object(cl_mem_out);
    dt_opencl_unlock_device(pipe->devid); // release opencl resource
    dt_pthread_mutex_lock(&pipe->busy_mutex);
    pipe->opencl_enabled = FALSE; // disable opencl for this pipe
    pipe->opencl_error = FALSE;   // reset error status
    pipe->devid = -1;
    dt_pthread_mutex_unlock(&pipe->busy_mutex);

    darktable.opencl->error_count++; // increase error count
    if(darktable.opencl->error_count >= DT_OPENCL_MAX_ERRORS)
    {
      // too frequent opencl errors encountered: this is a clear sign
      // of a broken setup. give up on opencl during this session.
      darktable.opencl->stopped = TRUE;
      dt_print(DT_DEBUG_OPENCL,
               "[opencl] frequent opencl errors encountered; disabling"
               " opencl for this session!\n");
      dt_control_log
        (_("darktable discovered problems with your OpenCL setup;"
           " disabling OpenCL for this session!"));
      // also remove "opencl" from capabilities so that the preference entry is greyed out
      dt_capabilities_remove("opencl");
    }

    dt_dev_pixelpipe_cache_flush(pipe);
    dt_dev_pixelpipe_change(pipe, dev);

    dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_OPENCL,
      "pixelpipe restarting on CPU", pipe, NULL, &roi, &roi, "\n");

    goto restart; // try again (this time without opencl)
  }

  // release resources:
  if(pipe->forms)
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
    pipe->processing = FALSE;
    return TRUE;
  }

  // terminate
  dt_pthread_mutex_lock(&pipe->backbuf_mutex);
  pipe->backbuf_hash = dt_dev_pixelpipe_cache_hash(pipe->image.id, &roi, pipe, 0);
  pipe->backbuf = buf;
  pipe->backbuf_width = width;
  pipe->backbuf_height = height;

  if(pipe->type & (DT_DEV_PIXELPIPE_PREVIEW
                   | DT_DEV_PIXELPIPE_FULL
                   | DT_DEV_PIXELPIPE_PREVIEW2))
  {
    if(pipe->output_backbuf == NULL
       || pipe->output_backbuf_width != pipe->backbuf_width
       || pipe->output_backbuf_height != pipe->backbuf_height)
    {
      g_free(pipe->output_backbuf);
      pipe->output_backbuf_width = pipe->backbuf_width;
      pipe->output_backbuf_height = pipe->backbuf_height;
      pipe->output_backbuf =
        g_malloc0(sizeof(uint8_t) * 4 * pipe->output_backbuf_width * pipe->output_backbuf_height);
    }

    if(pipe->output_backbuf)
      memcpy(pipe->output_backbuf, pipe->backbuf,
             sizeof(uint8_t) * 4 * pipe->output_backbuf_width * pipe->output_backbuf_height);
    pipe->output_imgid = pipe->image.id;
  }
  dt_pthread_mutex_unlock(&pipe->backbuf_mutex);

  dt_dev_pixelpipe_cache_report(pipe);

  dt_print_pipe(DT_DEBUG_PIPE, "pixelpipe finished", pipe, NULL, &roi, &roi, "\n\n");

  pipe->processing = FALSE;
  return FALSE;
}

void dt_dev_pixelpipe_get_dimensions(dt_dev_pixelpipe_t *pipe,
                                     struct dt_develop_t *dev,
                                     const int width_in,
                                     const int height_in,
                                     int *width,
                                     int *height)
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
       && !(dev->gui_module && dev->gui_module != module
            && dev->gui_module->operation_tags_filter() & module->operation_tags()))
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

/* this looks for a raster mask (mask output) generated by raster_mask_source, the size of
   the mask must now be equal to the roi_out of the requesting (target_module) module.

   As the raster mask was generated with roi_out size of the source module we always have
   to check for a necessary transformation by all modules between in the pixelpipe.

   The functions returns a pointer the the mask data or NULL if none was available.
   Also the boolean at free_mask is set to TRUE if mask has been somehow transformed,
   all callers must check this flag and de-allocate after usage (dt_free_align).
*/

float *dt_dev_get_raster_mask(const struct dt_dev_pixelpipe_iop_t *piece,
                              const dt_iop_module_t *raster_mask_source,
                              const dt_mask_id_t raster_mask_id,
                              const dt_iop_module_t *target_module,
                              gboolean *free_mask)
{
  if(!raster_mask_source)
    return NULL;

  *free_mask = FALSE;
  float *raster_mask = NULL;

  GList *source_iter;
  for(source_iter = piece->pipe->nodes; source_iter; source_iter = g_list_next(source_iter))
  {
    const dt_dev_pixelpipe_iop_t *candidate = (dt_dev_pixelpipe_iop_t *)source_iter->data;

    if((candidate->module == target_module)
       || (candidate->module->iop_order >= target_module->iop_order))
    {
      dt_control_log
        (_("module `%s' can't get raster mask from module `%s'\n"
           "as that is processed later in the pixel pipe.\n"
           "raster mask is ignored."),
         target_module->name(), raster_mask_source->name());

      dt_print(DT_DEBUG_ALWAYS,
               "module `%s' can't get raster mask id=%i from module `%s'"
               " as that is processed later in the pixel pipe\n",
               target_module->name(), raster_mask_id, raster_mask_source->name());
      return NULL;
    }

    if(candidate->module == raster_mask_source)
      break;
  }

  // we found the raster_mask source module
  if(source_iter)
  {
    const dt_dev_pixelpipe_iop_t *source_piece =
      (dt_dev_pixelpipe_iop_t *)source_iter->data;

    const gboolean source_enabled = source_piece && source_piece->enabled;
/* there might be stale masks from disabled modules left over.
   don't use those!
*/
    if(!source_enabled)
    {
      dt_print_pipe(DT_DEBUG_PIPE,
         "no raster found", piece->pipe, piece->module, NULL, NULL,
         "source module `%s%s' is disabled\n",
         raster_mask_source->name(), dt_iop_get_instance_name(raster_mask_source));
      return NULL;
    }
    else
    {
      raster_mask = g_hash_table_lookup(source_piece->raster_masks,
                                        GINT_TO_POINTER(raster_mask_id));
      if(!raster_mask)
      {
        dt_print_pipe(DT_DEBUG_PIPE,
          "no raster mask found", piece->pipe, piece->module, NULL, NULL,
          "raster mask seems to be lost in module `%s%s'\n",
          raster_mask_source->name(), dt_iop_get_instance_name(raster_mask_source));
        return NULL;
      }
      else
      {
        for(GList *iter = g_list_next(source_iter); iter; iter = g_list_next(iter))
        {
          dt_dev_pixelpipe_iop_t *module = (dt_dev_pixelpipe_iop_t *)iter->data;

          if(module->enabled
             && !(module->module->dev->gui_module
                  && module->module->dev->gui_module != module->module
                  && (module->module->dev->gui_module->operation_tags_filter()
                      & module->module->operation_tags())))
          {
            if(module->module->distort_mask
              // hack against pipes not using finalscale
              && !(dt_iop_module_is(module->module->so, "finalscale")
                    && module->processed_roi_in.width == 0
                    && module->processed_roi_in.height == 0))
            {
              float *transformed_mask =
                dt_alloc_align_float((size_t)module->processed_roi_out.width
                                     * module->processed_roi_out.height);
              if(transformed_mask)
              {
                module->module->distort_mask(module->module,
                                             module,
                                             raster_mask,
                                             transformed_mask,
                                             &module->processed_roi_in,
                                             &module->processed_roi_out);

                // As we might have multiple modules doing a transformation we want to
                // de-allocate all intermediate buffers and only leave the last one
                // to be used and deallocated by the caller.
                if(*free_mask) dt_free_align(raster_mask);

                *free_mask = TRUE;
                raster_mask = transformed_mask;
              }
              else
              {
                dt_print_pipe(DT_DEBUG_ALWAYS,
                      "no distort raster mask",
                      piece->pipe, module->module,
                      &module->processed_roi_in, &module->processed_roi_out,
                      "skipped transforming mask due to lack of memory\n");
                return NULL;
              }
            }
            else if(!module->module->distort_mask
                    && (module->processed_roi_in.width != module->processed_roi_out.width
                        || module->processed_roi_in.height != module->processed_roi_out.height
                        || module->processed_roi_in.x != module->processed_roi_out.x
                        || module->processed_roi_in.y != module->processed_roi_out.y))
            {
              dt_print_pipe(DT_DEBUG_ALWAYS,
                      "distort raster mask",
                      piece->pipe, module->module,
                      &module->processed_roi_in, &module->processed_roi_out,
                      "misses distort_mask() function\n");
              return NULL;
            }
          }

          if(module->module == target_module)
            break;
        }
      }
    }
  }

  dt_print_pipe(DT_DEBUG_PIPE,
      "got raster mask", piece->pipe, target_module, NULL, NULL,
      "from module `%s%s' %s\n",
      raster_mask_source->name(), dt_iop_get_instance_name(raster_mask_source),
      *free_mask ? "distorted" : "");

  return raster_mask;
}

void dt_dev_clear_rawdetail_mask(dt_dev_pixelpipe_t *pipe)
{
  if(pipe->rawdetail_mask_data) dt_free_align(pipe->rawdetail_mask_data);
  pipe->rawdetail_mask_data = NULL;
  pipe->detail_width = pipe->detail_height = 0;
}

gboolean dt_dev_write_rawdetail_mask(dt_dev_pixelpipe_iop_t *piece,
                                     float *const rgb,
                                     const dt_iop_roi_t *const roi_in,
                                     const gboolean rawmode)
{
  dt_dev_pixelpipe_t *p = piece->pipe;
  dt_dev_clear_rawdetail_mask(p);

  const int width = roi_in->width;
  const int height = roi_in->height;
  float *mask = dt_alloc_align_float((size_t)width * height);
  float *tmp = dt_alloc_align_float((size_t)width * height);
  if((mask == NULL) || (tmp == NULL)) goto error;

  p->rawdetail_mask_data = mask;
  p->detail_width = roi_in->width;
  p->detail_height = roi_in->height;

  const gboolean wboff = !p->dsc.temperature.enabled || !rawmode;
  const dt_aligned_pixel_t wb = { wboff ? 1.0f : p->dsc.temperature.coeffs[0],
                                  wboff ? 1.0f : p->dsc.temperature.coeffs[1],
                                  wboff ? 1.0f : p->dsc.temperature.coeffs[2]};
  dt_masks_calc_rawdetail_mask(rgb, mask, tmp, width, height, wb);
  dt_free_align(tmp);
  dt_print_pipe(DT_DEBUG_PIPE, "write detail mask on CPU", p, NULL, roi_in, NULL, "\n");
  return FALSE;

  error:
  dt_print(DT_DEBUG_ALWAYS, "[dt_dev_write_rawdetail_mask] couldn't write detail mask\n");
  dt_free_align(mask);
  dt_free_align(tmp);
  return TRUE;
}

#ifdef HAVE_OPENCL
gboolean dt_dev_write_rawdetail_mask_cl(dt_dev_pixelpipe_iop_t *piece,
                                        cl_mem in,
                                        const dt_iop_roi_t *const roi_in,
                                        const gboolean rawmode)
{
  dt_dev_pixelpipe_t *p = piece->pipe;
  dt_dev_clear_rawdetail_mask(p);

  const int width = roi_in->width;
  const int height = roi_in->height;

  cl_mem out = NULL;
  cl_mem tmp = NULL;
  float *mask = NULL;
  const int devid = p->devid;

  cl_int err = CL_SUCCESS;
  mask = dt_alloc_align_float((size_t)width * height);
  if(mask == NULL) goto error;
  out = dt_opencl_alloc_device(devid, width, height, sizeof(float));
  if(out == NULL) goto error;
  tmp = dt_opencl_alloc_device_buffer(devid, sizeof(float) * width * height);
  if(tmp == NULL) goto error;

  {
    const int kernel = darktable.opencl->blendop->kernel_calc_Y0_mask;
    const gboolean wboff = !p->dsc.temperature.enabled || !rawmode;

    const dt_aligned_pixel_t wb =
      { wboff ? 1.0f : p->dsc.temperature.coeffs[0],
        wboff ? 1.0f : p->dsc.temperature.coeffs[1],
        wboff ? 1.0f : p->dsc.temperature.coeffs[2]};

    err = dt_opencl_enqueue_kernel_2d_args
      (devid, kernel, width, height,
       CLARG(tmp), CLARG(in), CLARG(width), CLARG(height),
       CLARG(wb[0]), CLARG(wb[1]), CLARG(wb[2]));
    if(err != CL_SUCCESS) goto error;
  }
  {
    const int kernel = darktable.opencl->blendop->kernel_write_scharr_mask;
    err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, width, height,
      CLARG(tmp), CLARG(out), CLARG(width), CLARG(height));
    if(err != CL_SUCCESS) goto error;
  }

  {
    err = dt_opencl_read_host_from_device(devid, mask, out, width, height, sizeof(float));
    if(err != CL_SUCCESS) goto error;
  }

  p->rawdetail_mask_data = mask;
  p->detail_width = roi_in->width;
  p->detail_height = roi_in->height;

  dt_opencl_release_mem_object(out);
  dt_opencl_release_mem_object(tmp);
  dt_print_pipe(DT_DEBUG_PIPE, "write detail mask on GPU", p, NULL, roi_in, NULL, "\n");
  return FALSE;

  error:
  dt_print(DT_DEBUG_ALWAYS,
           "[dt_dev_write_rawdetail_mask_cl] couldn't write detail mask: %s\n", cl_errstr(err));
  dt_dev_clear_rawdetail_mask(p);
  dt_opencl_release_mem_object(out);
  dt_opencl_release_mem_object(tmp);
  dt_free_align(mask);
  return TRUE;
}
#endif

// this expects a mask prepared by the demosaicer and distorts the
// mask through all pipeline modules until target
float *dt_dev_distort_detail_mask(dt_dev_pixelpipe_t *pipe,
                                  float *src,
                                  const dt_iop_module_t *target_module)
{
  gboolean valid = FALSE;
  const gboolean raw_img = dt_image_is_raw(&pipe->image);

  GList *source_iter;
  for(source_iter = pipe->nodes; source_iter; source_iter = g_list_next(source_iter))
  {
    const dt_dev_pixelpipe_iop_t *candidate = (dt_dev_pixelpipe_iop_t *)source_iter->data;
    if(dt_iop_module_is(candidate->module->so, "demosaic")
       && candidate->enabled
       && raw_img)
    {
      valid = TRUE;
      break;
    }
    if(dt_iop_module_is(candidate->module->so, "rawprepare")
       && candidate->enabled
       && !raw_img)
    {
      valid = TRUE;
      break;
    }
  }

  if(!valid) return NULL;
  dt_print(DT_DEBUG_MASKS | DT_DEBUG_VERBOSE,
           "[dt_dev_distort_detail_mask] (%ix%i) for module %s\n",
           pipe->detail_width, pipe->detail_height,
           target_module->op);

  float *resmask = src;
  float *inmask  = src;
  if(source_iter)
  {
    for(GList *iter = source_iter; iter; iter = g_list_next(iter))
    {
      dt_dev_pixelpipe_iop_t *module = (dt_dev_pixelpipe_iop_t *)iter->data;
      if(module->enabled
         && !(module->module->dev->gui_module
              && module->module->dev->gui_module != module->module
              && module->module->dev->gui_module->operation_tags_filter()
                 & module->module->operation_tags()))
      {
        // hack against pipes not using finalscale
        if(module->module->distort_mask
              && !(dt_iop_module_is(module->module->so, "finalscale")
                    && module->processed_roi_in.width == 0
                    && module->processed_roi_in.height == 0))
        {
          float *tmp = dt_alloc_align_float((size_t)module->processed_roi_out.width
                                            * module->processed_roi_out.height);
          dt_print(DT_DEBUG_MASKS | DT_DEBUG_VERBOSE,
                   "   %s %ix%i -> %ix%i\n",
                   module->module->op,
                   module->processed_roi_in.width, module->processed_roi_in.height,
                   module->processed_roi_out.width, module->processed_roi_out.height);

          module->module->distort_mask(module->module, module, inmask, tmp,
                                       &module->processed_roi_in,
                                       &module->processed_roi_out);
          resmask = tmp;
          if(inmask != src) dt_free_align(inmask);
          inmask = tmp;
        }
        else if(!module->module->distort_mask
                && (module->processed_roi_in.width != module->processed_roi_out.width
                    || module->processed_roi_in.height != module->processed_roi_out.height
                    || module->processed_roi_in.x != module->processed_roi_out.x
                    || module->processed_roi_in.y != module->processed_roi_out.y))
              dt_print_pipe(DT_DEBUG_ALWAYS,
                      "distort raster mask",
                      pipe, module->module,
                      &module->processed_roi_in, &module->processed_roi_out,
                      "misses distort_mask()\n");

        if(module->module == target_module) break;
      }
    }
  }
  return resmask;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
