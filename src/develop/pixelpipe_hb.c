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
#include "develop/pixelpipe.h"
#include "gui/gtk.h"
#include "control/control.h"

#include <assert.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>

// cache line resolution
#define DT_DEV_PIXELPIPE_CACHE_SIZE DT_IMAGE_WINDOW_SIZE

// this is to ensure compatibility with pixelpipe_gegl.c, which does not need to build the other module:
#include "develop/pixelpipe_cache.c"

void dt_dev_pixelpipe_init_export(dt_dev_pixelpipe_t *pipe, int32_t width, int32_t height)
{
  dt_dev_pixelpipe_init_cached(pipe, 3*sizeof(float)*width*height, 2);
  pipe->type = DT_DEV_PIXELPIPE_EXPORT;
}

void dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_init_cached(pipe, 3*sizeof(float)*DT_DEV_PIXELPIPE_CACHE_SIZE*DT_DEV_PIXELPIPE_CACHE_SIZE, 5);
  pipe->type = DT_DEV_PIXELPIPE_FULL;
}

void dt_dev_pixelpipe_init_cached(dt_dev_pixelpipe_t *pipe, int32_t size, int32_t entries)
{
  pipe->changed = DT_DEV_PIPE_UNCHANGED;
  pipe->processed_width  = pipe->backbuf_width  = pipe->iwidth = 0;
  pipe->processed_height = pipe->backbuf_height = pipe->iheight = 0;
  pipe->nodes = NULL;
  pipe->backbuf_size = size;
  dt_dev_pixelpipe_cache_init(&(pipe->cache), entries, pipe->backbuf_size);
  pipe->backbuf = NULL;
  pipe->processing = 0;
  pipe->shutdown = 0;
  pipe->input_timestamp = 0;
  pthread_mutex_init(&(pipe->backbuf_mutex), NULL);
  pthread_mutex_init(&(pipe->busy_mutex), NULL);
}

void dt_dev_pixelpipe_set_input(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, float *input, int width, int height, float iscale)
{
  pipe->iwidth  = width;
  pipe->iheight = height;
  pipe->iscale = iscale;
  pipe->input = input;
  if(width < dev->image->width && height < dev->image->height) pipe->type = DT_DEV_PIXELPIPE_PREVIEW;
}

void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe)
{
  pthread_mutex_lock(&pipe->backbuf_mutex);
  pipe->backbuf = NULL;
  // blocks while busy and sets shutdown bit:
  dt_dev_pixelpipe_cleanup_nodes(pipe);
  // so now it's safe to clean up cache:
  dt_dev_pixelpipe_cache_cleanup(&(pipe->cache));
  pthread_mutex_unlock(&pipe->backbuf_mutex);
  pthread_mutex_destroy(&(pipe->backbuf_mutex));
  pthread_mutex_destroy(&(pipe->busy_mutex));
}

void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe)
{
  pthread_mutex_lock(&pipe->busy_mutex);
  pipe->shutdown = 1;
  // destroy all nodes
  GList *nodes = pipe->nodes;
  while(nodes)
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    // printf("cleanup module `%s'\n", piece->module->name());
    piece->module->cleanup_pipe(piece->module, pipe, piece);
    free(piece);
    nodes = g_list_next(nodes);
  }
  g_list_free(pipe->nodes);
  pipe->nodes = NULL;
  pthread_mutex_unlock(&pipe->busy_mutex);
}

void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  pthread_mutex_lock(&pipe->busy_mutex);
  pipe->shutdown = 0;
  g_assert(pipe->nodes == NULL);
  // for all modules in dev:
  GList *modules = dev->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    // if(module->enabled) // no! always create nodes. just don't process.
    {
      dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)malloc(sizeof(dt_dev_pixelpipe_iop_t));
      piece->enabled = module->enabled;
      piece->iscale  = pipe->iscale;
      piece->iwidth  = pipe->iwidth;
      piece->iheight = pipe->iheight;
      piece->module  = module;
      piece->pipe    = pipe;
      piece->data = NULL;
      piece->hash = 0;
      piece->module->init_pipe(piece->module, pipe, piece);
      pipe->nodes = g_list_append(pipe->nodes, piece);
    }
    modules = g_list_next(modules);
  }
  pthread_mutex_unlock(&pipe->busy_mutex);
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
      dt_iop_commit_params(hist->module, hist->params, pipe, piece);
    }
    nodes = g_list_next(nodes);
  }
}

void dt_dev_pixelpipe_synch_all(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  pthread_mutex_lock(&pipe->busy_mutex);
  // call reset_params on all pieces first.
  GList *nodes = pipe->nodes;
  while(nodes)
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    piece->hash = 0;
    piece->enabled = piece->module->default_enabled;
    dt_iop_commit_params(piece->module, piece->module->default_params, pipe, piece);
    nodes = g_list_next(nodes);
  }
  // go through all history items and adjust params
  GList *history = dev->history;
  for(int k=0;k<dev->history_end;k++)
  {
    dt_dev_pixelpipe_synch(pipe, dev, history);
    history = g_list_next(history);
  }
  pthread_mutex_unlock(&pipe->busy_mutex);
}

void dt_dev_pixelpipe_synch_top(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  pthread_mutex_lock(&pipe->busy_mutex);
  GList *history = g_list_nth(dev->history, dev->history_end - 1);
  if(history) dt_dev_pixelpipe_synch(pipe, dev, history);
  pthread_mutex_unlock(&pipe->busy_mutex);
}

void dt_dev_pixelpipe_change(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev)
{
  pthread_mutex_lock(&dev->history_mutex);
  // case DT_DEV_PIPE_UNCHANGED: case DT_DEV_PIPE_ZOOMED:
  if(pipe->changed & DT_DEV_PIPE_TOP_CHANGED)
  { // only top history item changed.
    dt_dev_pixelpipe_synch_top(pipe, dev);
  }
  if(pipe->changed & DT_DEV_PIPE_SYNCH)
  { // pipeline topology remains intact, only change all params.
    dt_dev_pixelpipe_synch_all(pipe, dev);
  }
  if(pipe->changed & DT_DEV_PIPE_REMOVE)
  { // modules have been added in between or removed. need to rebuild the whole pipeline.
    dt_dev_pixelpipe_cleanup_nodes(pipe);
    dt_dev_pixelpipe_create_nodes(pipe, dev);
  }
  pipe->changed = DT_DEV_PIPE_UNCHANGED;
  pthread_mutex_unlock(&dev->history_mutex);
  if(pipe == dev->preview_pipe)
    dt_dev_pixelpipe_get_dimensions(pipe, dev, dev->mipf_exact_width, dev->mipf_exact_height, &pipe->processed_width, &pipe->processed_height);
  else
    dt_dev_pixelpipe_get_dimensions(pipe, dev, pipe->iwidth, pipe->iheight, &pipe->processed_width, &pipe->processed_height);
}

// TODO:
void dt_dev_pixelpipe_add_node(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int n)
{
}
// TODO:
void dt_dev_pixelpipe_remove_node(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int n)
{
}

// recursive helper for process:
int dt_dev_pixelpipe_process_rec(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, void **output, const dt_iop_roi_t *roi_out, GList *modules, GList *pieces, int pos)
{
  dt_iop_roi_t roi_in = *roi_out;
  double start, end;

  void *input = NULL;
  dt_iop_module_t *module = NULL;
  dt_dev_pixelpipe_iop_t *piece = NULL;
  if(modules)
  {
    module = (dt_iop_module_t *)modules->data;
    piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
    // skip this module?
    if(!piece->enabled)
    {
      // printf("skipping disabled module %s\n", module->op);
      return dt_dev_pixelpipe_process_rec(pipe, dev, output, &roi_in, g_list_previous(modules), g_list_previous(pieces), pos-1);
    }
  }

  // if available, return data
  pthread_mutex_lock(&pipe->busy_mutex);
  if(pipe->shutdown) { pthread_mutex_unlock(&pipe->busy_mutex); return 1; }
  uint64_t hash = dt_dev_pixelpipe_cache_hash(dev->image->id, roi_out, pipe, pos);
  if(dt_dev_pixelpipe_cache_available(&(pipe->cache), hash))
  {
    // if(module) printf("found valid buf pos %d in cache for module %s %s %lu\n", pos, module->op, pipe == dev->preview_pipe ? "[preview]" : "", hash);
    (void) dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, output);
    pthread_mutex_unlock(&pipe->busy_mutex);
    if(!modules) return 0;
    // go to post-collect directly:
    goto post_process_collect_info;
  }
  else pthread_mutex_unlock(&pipe->busy_mutex);

  // if history changed or exit event, abort processing?
  // preview pipe: abort on all but zoom events (same buffer anyways)
  if(pipe != dev->preview_pipe)
  {
    if(pipe != dev->preview_pipe && pipe->changed == DT_DEV_PIPE_ZOOMED) return 1;
    if((pipe->changed != DT_DEV_PIPE_UNCHANGED && pipe->changed != DT_DEV_PIPE_ZOOMED) || dev->gui_leaving) return 1;
  }
  // if image has changed, stop now.
  if(pipe == dev->pipe && dev->image_force_reload) return 1;
  if(pipe == dev->preview_pipe && dev->preview_loading) return 1;
  if(pipe == dev->preview_pipe && pipe->input != dev->image->mipf) return 1;
  if(dev->gui_leaving) return 1;

  // input -> output
  if(!modules)
  { // import input array with given scale and roi
    pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown) { pthread_mutex_unlock(&pipe->busy_mutex); return 1; }
    // printf("[process] loading source image buffer\n");
    start = dt_get_wtime();
#if 1
    *output = pipe->input;
#else
    // optimized branch (for mipf-preview):
    if(roi_out->scale == 1.0 && roi_out->x == 0 && roi_out->y == 0 && pipe->iwidth == roi_out->width && pipe->iheight == roi_out->height) *output = pipe->input;
    else
    {
      // printf("[process] scale/pan\n");
      // reserve new cache line: output
      if(dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, output))
      {
        bzero(*output, pipe->backbuf_size);
        if(roi_out->scale < 0.5)
        {
          dt_iop_clip_and_zoom_hq_downsample(pipe->input, roi_out->x/roi_out->scale, roi_out->y/roi_out->scale,
              roi_out->width/roi_out->scale, roi_out->height/roi_out->scale, pipe->iwidth, pipe->iheight,
              *output, 0, 0, roi_out->width, roi_out->height, roi_out->width, roi_out->height);
        }
        else
        {
          dt_iop_clip_and_zoom(pipe->input, roi_out->x/roi_out->scale, roi_out->y/roi_out->scale,
              roi_out->width/roi_out->scale, roi_out->height/roi_out->scale, pipe->iwidth, pipe->iheight,
              *output, 0, 0, roi_out->width, roi_out->height, roi_out->width, roi_out->height);
        }
      }
    }
#endif
    end = dt_get_wtime();
    dt_print(DT_DEBUG_PERF, "[dev_pixelpipe] took %.3f secs initing base buffer\n", end - start);
    pthread_mutex_unlock(&pipe->busy_mutex);
  }
  else
  {
    // recurse and obtain output array in &input
    pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown) { pthread_mutex_unlock(&pipe->busy_mutex); return 1; }
    module->modify_roi_in(module, piece, roi_out, &roi_in);
    pthread_mutex_unlock(&pipe->busy_mutex);
    // check roi_in for sanity and clip to maximum alloc'ed area, downscale
    // input if necessary.
    roi_in.scale = fabsf(roi_in.scale);
    if(roi_in.x < 0) roi_in.x = 0;
    if(roi_in.y < 0) roi_in.y = 0;
    if(roi_in.width  < 1) roi_in.width  = 1;
    if(roi_in.height < 1) roi_in.height = 1;

#if 0 // zoom 2:1 actually breaks this check:
    // clamp max width:
    if(roi_in.width +roi_in.x > pipe->iwidth *roi_in.scale) roi_in.width  = pipe->iwidth *roi_in.scale - roi_in.x;
    if(roi_in.height+roi_in.y > pipe->iheight*roi_in.scale) roi_in.height = pipe->iheight*roi_in.scale - roi_in.y;
#endif

    int maxwd = DT_DEV_PIXELPIPE_CACHE_SIZE;
    int maxht = DT_DEV_PIXELPIPE_CACHE_SIZE;
    // downscale request to cache buffer:
    if(pipe->type == DT_DEV_PIXELPIPE_EXPORT)
    {
      maxwd = pipe->iwidth;
      maxht = pipe->iheight;
    }
    // only necessary if mem area would overflow:
    if(roi_in.width*roi_in.height > maxwd*maxht)
    {
      if(roi_in.width  > maxwd)
      {
        const float f = maxwd/(float)roi_in.width;
        roi_in.scale  *= f;
        roi_in.height *= f;
        roi_in.x      *= f;
        roi_in.y      *= f;
        roi_in.width   = maxwd;
      }
      if(roi_in.height > maxht)
      {
        const float f  = maxht/(float)roi_in.height;
        roi_in.scale  *= f;
        roi_in.x      *= f;
        roi_in.y      *= f;
        roi_in.width  *= f;
        roi_in.height  = maxht;
      }
    }

    if(dt_dev_pixelpipe_process_rec(pipe, dev, &input, &roi_in, g_list_previous(modules), g_list_previous(pieces), pos-1)) return 1;
    piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
    // reserve new cache line: output
    pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown) { pthread_mutex_unlock(&pipe->busy_mutex); return 1; }
    if(!strcmp(module->op, "gamma"))
      (void) dt_dev_pixelpipe_cache_get_important(&(pipe->cache), hash, output);
    else
      (void) dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, output);
    pthread_mutex_unlock(&pipe->busy_mutex);

    // if(module) printf("reserving new buf in cache for module %s %s: %ld buf %lX\n", module->op, pipe == dev->preview_pipe ? "[preview]" : "", hash, (long int)*output);
    
    // tonecurve histogram (collect luminance only):
    pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown) { pthread_mutex_unlock(&pipe->busy_mutex); return 1; }
    if(dev->gui_attached && pipe == dev->preview_pipe && (strcmp(module->op, "tonecurve") == 0))
    {
      float *pixel = (float *)input;
      dev->histogram_pre_max = 0;
      bzero(dev->histogram_pre, sizeof(float)*4*64);
      for(int j=0;j<roi_in.height;j+=3) for(int i=0;i<roi_in.width;i+=3)
      {
        uint8_t L = CLAMP(63/100.0*(pixel[3*j*roi_in.width+3*i]), 0, 63);
        dev->histogram_pre[4*L+3] ++;
      }
      // don't count <= 0 pixels
      for(int k=3;k<4*64;k+=4) dev->histogram_pre[k] = logf(1.0 + dev->histogram_pre[k]);
      for(int k=19;k<4*64;k+=4) dev->histogram_pre_max = dev->histogram_pre_max > dev->histogram_pre[k] ? dev->histogram_pre_max : dev->histogram_pre[k];
      pthread_mutex_unlock(&pipe->busy_mutex);
      dt_control_queue_draw(module->widget);
    }
    else pthread_mutex_unlock(&pipe->busy_mutex);

    // if module requested color statistics, get mean in box from preview pipe
    pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown) { pthread_mutex_unlock(&pipe->busy_mutex); return 1; }
    if(!(dev->image->flags & DT_IMAGE_THUMBNAIL) && // converted jpg is useless.
        dev->gui_attached && pipe == dev->preview_pipe && // pick from preview pipe to get pixels outside the viewport
        (module == dev->gui_module || !strcmp(module->op, "colorout")) && // only modules with focus or colorout for bottom panel can pick
        module->request_color_pick) // and they need to want to pick ;)
    {
      for(int k=0;k<3;k++) module->picked_color_min[k] =  666.0f;
      for(int k=0;k<3;k++) module->picked_color_max[k] = -666.0f;
      for(int k=0;k<3;k++) module->picked_color_min_Lab[k] =  666.0f;
      for(int k=0;k<3;k++) module->picked_color_max_Lab[k] = -666.0f;
      int box[4];
      float rgb[3], Lab[3], *in = (float *)input;
      for(int k=0;k<3;k++) Lab[k] = rgb[k] = 0.0f;
      for(int k=0;k<4;k+=2) box[k] = MIN(roi_in.width -1, MAX(0, module->color_picker_box[k]*roi_in.width));
      for(int k=1;k<4;k+=2) box[k] = MIN(roi_in.height-1, MAX(0, module->color_picker_box[k]*roi_in.height));
      const float w = 1.0/((box[3]-box[1]+1)*(box[2]-box[0]+1));
      for(int j=box[1];j<=box[3];j++) for(int i=box[0];i<=box[2];i++)
      {
        for(int k=0;k<3;k++)
        {
          module->picked_color_min[k] = fminf(module->picked_color_min[k], in[3*(roi_in.width*j + i) + k]);
          module->picked_color_max[k] = fmaxf(module->picked_color_max[k], in[3*(roi_in.width*j + i) + k]);
          rgb[k] += w*in[3*(roi_in.width*j + i) + k];
        }
        const float L = in[3*(roi_in.width*j + i) + 0];
        const float a = fminf(128, fmaxf(-128.0, in[3*(roi_in.width*j + i) + 1]*L));
        const float b = fminf(128, fmaxf(-128.0, in[3*(roi_in.width*j + i) + 2]*L));
        Lab[1] += w*a;
        Lab[2] += w*b;
        module->picked_color_min_Lab[0] = fminf(module->picked_color_min_Lab[0], L);
        module->picked_color_min_Lab[1] = fminf(module->picked_color_min_Lab[1], a);
        module->picked_color_min_Lab[2] = fminf(module->picked_color_min_Lab[2], b);
        module->picked_color_max_Lab[0] = fmaxf(module->picked_color_max_Lab[0], L);
        module->picked_color_max_Lab[1] = fmaxf(module->picked_color_max_Lab[1], a);
        module->picked_color_max_Lab[2] = fmaxf(module->picked_color_max_Lab[2], b);
      }
      Lab[0] = rgb[0];
      for(int k=0;k<3;k++) module->picked_color_Lab[k] = Lab[k];
      for(int k=0;k<3;k++) module->picked_color[k] = rgb[k];

      pthread_mutex_unlock(&pipe->busy_mutex);
      int needlock = pthread_self() != darktable.control->gui_thread;
      if(needlock) gdk_threads_enter();
      gtk_widget_queue_draw(module->widget);
      if(needlock) gdk_threads_leave();
    }
    else pthread_mutex_unlock(&pipe->busy_mutex);

    // printf("%s processing %s\n", pipe == dev->preview_pipe ? "[preview]" : "", module->op);
    // actual pixel processing done by module
    pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown) { pthread_mutex_unlock(&pipe->busy_mutex); return 1; }
    start = dt_get_wtime();
    module->process(module, piece, input, *output, &roi_in, roi_out);
    end = dt_get_wtime();
    dt_print(DT_DEBUG_PERF, "[dev_pixelpipe] took %.3f secs processing `%s' [%s]\n", end - start, module->name(),
        pipe->type == DT_DEV_PIXELPIPE_PREVIEW ? "preview" : (pipe->type == DT_DEV_PIXELPIPE_FULL ? "full" : "export"));
    pthread_mutex_unlock(&pipe->busy_mutex);
#ifdef _DEBUG
    pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown) { pthread_mutex_unlock(&pipe->busy_mutex); return 1; }
    if(strcmp(module->op, "gamma")) for(int k=0;k<3*roi_out->width*roi_out->height;k++)
    {
      if(!isfinite(((float*)(*output))[k]))
      {
        fprintf(stderr, "[dev_pixelpipe] module `%s' outputs non-finite floats!\n", module->name());
        break;
      }
    }
    pthread_mutex_unlock(&pipe->busy_mutex);
#endif

post_process_collect_info:
    // final histogram:
    pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown) { pthread_mutex_unlock(&pipe->busy_mutex); return 1; }
    if(dev->gui_attached && pipe == dev->preview_pipe && (strcmp(module->op, "gamma") == 0))
    {
      uint8_t *pixel = (uint8_t *)*output;
      dev->histogram_max = 0;
      bzero(dev->histogram, sizeof(float)*4*64);
      for(int j=0;j<roi_out->height;j+=4) for(int i=0;i<roi_out->width;i+=4)
      {
        uint8_t rgb[3];
        for(int k=0;k<3;k++)
          rgb[k] = pixel[4*j*roi_out->width+4*i+2-k]>>2;

        for(int k=0;k<3;k++)
          dev->histogram[4*rgb[k]+k] ++;
        uint8_t lum = MAX(MAX(rgb[0], rgb[1]), rgb[2]);
        dev->histogram[4*lum+3] ++;
      }
      for(int k=0;k<4*64;k++) dev->histogram[k] = logf(1.0 + dev->histogram[k]);
      // don't count <= 0 pixels
      for(int k=19;k<4*64;k+=4) dev->histogram_max = dev->histogram_max > dev->histogram[k] ? dev->histogram_max : dev->histogram[k];
      pthread_mutex_unlock(&pipe->busy_mutex);
      dt_control_queue_draw(glade_xml_get_widget (darktable.gui->main_window, "histogram"));
    }
    else pthread_mutex_unlock(&pipe->busy_mutex);
  }
  return 0;
}


int dt_dev_pixelpipe_process_no_gamma(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int x, int y, int width, int height, float scale)
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

int dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int x, int y, int width, int height, float scale)
{
  pipe->processing = 1;
  for(int k=0;k<3;k++) pipe->processed_maximum[k] = dev->image->maximum;
  dt_iop_roi_t roi = (dt_iop_roi_t){x, y, width, height, scale};
  // printf("pixelpipe homebrew process start\n");
  // dt_dev_pixelpipe_cache_print(&pipe->cache);

  //  go through list of modules from the end:
  int pos = g_list_length(dev->iop);
  GList *modules = g_list_last(dev->iop);
  GList *pieces = g_list_last(pipe->nodes);
  void *buf = NULL;
  if(dt_dev_pixelpipe_process_rec(pipe, dev, &buf, &roi, modules, pieces, pos)) return 1;
  pthread_mutex_lock(&pipe->backbuf_mutex);
  pipe->backbuf_hash = dt_dev_pixelpipe_cache_hash(dev->image->id, &roi, pipe, 0);
  pipe->backbuf = buf;
  pipe->backbuf_width  = width;
  pipe->backbuf_height = height;
  pthread_mutex_unlock(&pipe->backbuf_mutex);

  // printf("pixelpipe homebrew process end\n");
  pipe->processing = 0;
  return 0;
}

void dt_dev_pixelpipe_flush_caches(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_flush(&pipe->cache);
}

void dt_dev_pixelpipe_get_dimensions(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, int width_in, int height_in, int *width, int *height)
{
  pthread_mutex_lock(&pipe->busy_mutex);
  dt_iop_roi_t roi_in = (dt_iop_roi_t){0, 0, width_in, height_in, 1.0};
  dt_iop_roi_t roi_out;
  GList *modules = dev->iop;
  GList *pieces  = pipe->nodes;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
    // skip this module?
    if(piece->enabled)
    {
      piece->buf_in = roi_in;
      module->modify_roi_out(module, piece, &roi_out, &roi_in);
      piece->buf_out = roi_out;
      roi_in = roi_out;
    }
    modules = g_list_next(modules);
    pieces = g_list_next(pieces);
  }
  *width  = roi_out.width;
  *height = roi_out.height;
  pthread_mutex_unlock(&pipe->busy_mutex);
}
