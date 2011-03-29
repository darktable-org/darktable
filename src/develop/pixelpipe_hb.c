/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2011 henrik andersson.

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
#include "develop/blend.h"
#include "gui/gtk.h"
#include "control/control.h"
#include "common/opencl.h"
#include "common/similarity.h"

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
  dt_dev_pixelpipe_init_cached(pipe, 4*sizeof(float)*width*height, 2);
  pipe->type = DT_DEV_PIXELPIPE_EXPORT;
}

void dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_init_cached(pipe, 4*sizeof(float)*DT_DEV_PIXELPIPE_CACHE_SIZE*DT_DEV_PIXELPIPE_CACHE_SIZE, 5);
  pipe->type = DT_DEV_PIXELPIPE_FULL;
}

void dt_dev_pixelpipe_init_cached(dt_dev_pixelpipe_t *pipe, int32_t size, int32_t entries)
{
  pipe->devid = -1;
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
  dt_pthread_mutex_init(&(pipe->backbuf_mutex), NULL);
  dt_pthread_mutex_init(&(pipe->busy_mutex), NULL);
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
  dt_pthread_mutex_lock(&pipe->backbuf_mutex);
  pipe->backbuf = NULL;
  // blocks while busy and sets shutdown bit:
  dt_dev_pixelpipe_cleanup_nodes(pipe);
  // so now it's safe to clean up cache:
  dt_dev_pixelpipe_cache_cleanup(&(pipe->cache));
  dt_pthread_mutex_unlock(&pipe->backbuf_mutex);
  dt_pthread_mutex_destroy(&(pipe->backbuf_mutex));
  dt_pthread_mutex_destroy(&(pipe->busy_mutex));
}

void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex);
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
  dt_pthread_mutex_unlock(&pipe->busy_mutex);
}

void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex);
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
      piece->colors  = 4;
      piece->iscale  = pipe->iscale;
      piece->iwidth  = pipe->iwidth;
      piece->iheight = pipe->iheight;
      piece->module  = module;
      piece->pipe    = pipe;
      piece->data = NULL;
      piece->hash = 0;
      dt_iop_init_pipe(piece->module, pipe,piece);
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
    dt_iop_commit_params(piece->module, piece->module->default_params, piece->module->default_blendop_params, pipe, piece);
    nodes = g_list_next(nodes);
  }
  // go through all history items and adjust params
  GList *history = dev->history;
  for(int k=0; k<dev->history_end; k++)
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
  }
  pipe->changed = DT_DEV_PIPE_UNCHANGED;
  dt_pthread_mutex_unlock(&dev->history_mutex);
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

static int
get_output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece, dt_develop_t *dev)
{
  if(!module)
  {
    // first input.
    // mipf and non-raw images have 4 floats per pixel
    if(pipe->type == DT_DEV_PIXELPIPE_PREVIEW || dev->image->filters == 0) return 4*sizeof(float);
    else return dev->image->bpp;
  }
  return module->output_bpp(module, pipe, piece);
}


// recursive helper for process:
static int
dt_dev_pixelpipe_process_rec(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, void **output, void **cl_mem_output, int *out_bpp,
                             const dt_iop_roi_t *roi_out, GList *modules, GList *pieces, int pos)
{
  dt_iop_roi_t roi_in = *roi_out;

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
    if(!piece->enabled)
      return dt_dev_pixelpipe_process_rec(pipe, dev, output, cl_mem_output, out_bpp, &roi_in, g_list_previous(modules), g_list_previous(pieces), pos-1);
  }

  const int bpp = get_output_bpp(module, pipe, piece, dev);
  *out_bpp = bpp;
  const size_t bufsize = bpp*roi_out->width*roi_out->height;


  // 1) if cached buffer is still available, return data
  dt_pthread_mutex_lock(&pipe->busy_mutex);
  if(pipe->shutdown)
  {
    dt_pthread_mutex_unlock(&pipe->busy_mutex);
    return 1;
  }
  uint64_t hash = dt_dev_pixelpipe_cache_hash(dev->image->id, roi_out, pipe, pos);
  if(dt_dev_pixelpipe_cache_available(&(pipe->cache), hash))
  {
    // if(module) printf("found valid buf pos %d in cache for module %s %s %lu\n", pos, module->op, pipe == dev->preview_pipe ? "[preview]" : "", hash);
    // copy over cached processed max for clipping:
    if(piece) for(int k=0; k<3; k++) pipe->processed_maximum[k] = piece->processed_maximum[k];
    else      for(int k=0; k<3; k++) pipe->processed_maximum[k] = 1.0f;
    (void) dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, bufsize, output);
    dt_pthread_mutex_unlock(&pipe->busy_mutex);
    if(!modules) return 0;
    // go to post-collect directly:
    goto post_process_collect_info;
  }
  else dt_pthread_mutex_unlock(&pipe->busy_mutex);


  // 2) if history changed or exit event, abort processing?
  // preview pipe: abort on all but zoom events (same buffer anyways)
  if(dt_iop_breakpoint(dev, pipe)) return 1;
  // if image has changed, stop now.
  if(pipe == dev->pipe && dev->image_force_reload) return 1;
  if(pipe == dev->preview_pipe && dev->preview_loading) return 1;
  if(pipe == dev->preview_pipe && pipe->input != dev->image->mipf) return 1;
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
    if(pipe->type != DT_DEV_PIXELPIPE_PREVIEW)
    {
      if(roi_out->scale == 1.0 && roi_out->x == 0 && roi_out->y == 0 && pipe->iwidth == roi_out->width && pipe->iheight == roi_out->height)
      {
        *output = pipe->input;
      }
      else if(dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, bufsize, output))
      {
        memset(*output, 0, pipe->backbuf_size);
        if(roi_in.scale == 1.0f)
        {
          // fast branch for 1:1 pixel copies.
#ifdef _OPENMP
          #pragma omp parallel for schedule(static) default(none) shared(pipe,roi_out,roi_in,output)
#endif
          for(int j=0; j<MIN(roi_out->height, pipe->iheight-roi_in.y); j++)
            memcpy(((char *)*output) + bpp*j*roi_out->width, ((char *)pipe->input) + bpp*(roi_in.x + (roi_in.y + j)*pipe->iwidth), bpp*roi_out->width);
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
    // optimized branch (for mipf-preview):
    else if(pipe->type == DT_DEV_PIXELPIPE_PREVIEW && roi_out->scale == 1.0 && roi_out->x == 0 && roi_out->y == 0 && pipe->iwidth == roi_out->width && pipe->iheight == roi_out->height) *output = pipe->input;
    else
    {
      // reserve new cache line: output
      if(dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, bufsize, output))
      {
        roi_in.x /= roi_out->scale;
        roi_in.y /= roi_out->scale;
        roi_in.width = pipe->iwidth;
        roi_in.height = pipe->iheight;
        roi_in.scale = 1.0f;
        dt_iop_clip_and_zoom(*output, pipe->input, roi_out, &roi_in, roi_out->width, pipe->iwidth);
      }
    }
    dt_show_times(&start, "[dev_pixelpipe]", "initing base buffer [%s]", pipe->type == DT_DEV_PIXELPIPE_PREVIEW ? "preview" : (pipe->type == DT_DEV_PIXELPIPE_FULL ? "full" : "export"));
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
    int in_bpp;
    if(dt_dev_pixelpipe_process_rec(pipe, dev, &input, &cl_mem_input, &in_bpp, &roi_in, g_list_previous(modules), g_list_previous(pieces), pos-1)) return 1;
    piece = (dt_dev_pixelpipe_iop_t *)pieces->data;

    // reserve new cache line: output
    dt_pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown)
    {
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      return 1;
    }
    if(!strcmp(module->op, "gamma"))
      (void) dt_dev_pixelpipe_cache_get_important(&(pipe->cache), hash, bufsize, output);
    else
      (void) dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, bufsize, output);
    dt_pthread_mutex_unlock(&pipe->busy_mutex);

    // if(module) printf("reserving new buf in cache for module %s %s: %ld buf %lX\n", module->op, pipe == dev->preview_pipe ? "[preview]" : "", hash, (long int)*output);

    // tonecurve histogram (collect luminance only):
    dt_pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown)
    {
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      return 1;
    }
    if(dev->gui_attached && pipe == dev->preview_pipe && (strcmp(module->op, "tonecurve") == 0))
    {
      float *pixel = (float *)input;
      dev->histogram_pre_max = 0;
      memset (dev->histogram_pre, 0, sizeof(float)*4*64);
      for(int j=0; j<roi_in.height; j+=4) for(int i=0; i<roi_in.width; i+=4)
        {
          uint8_t L = CLAMP(63/100.0*(pixel[4*j*roi_in.width+4*i]), 0, 63);
          dev->histogram_pre[4*L+3] ++;
        }
      // don't count <= 0 pixels
      for(int k=3; k<4*64; k+=4) dev->histogram_pre[k] = logf(1.0 + dev->histogram_pre[k]);
      for(int k=19; k<4*64; k+=4) dev->histogram_pre_max = dev->histogram_pre_max > dev->histogram_pre[k] ? dev->histogram_pre_max : dev->histogram_pre[k];
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      dt_control_queue_draw(module->widget);
    }
    else dt_pthread_mutex_unlock(&pipe->busy_mutex);

    // if module requested color statistics, get mean in box from preview pipe
    dt_pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown)
    {
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      return 1;
    }
    if(dev->gui_attached && pipe == dev->preview_pipe && // pick from preview pipe to get pixels outside the viewport
        (module == dev->gui_module || !strcmp(module->op, "colorout")) && // only modules with focus or colorout for bottom panel can pick
        module->request_color_pick) // and they need to want to pick ;)
    {
      for(int k=0; k<3; k++) module->picked_color_min[k] =  666.0f;
      for(int k=0; k<3; k++) module->picked_color_max[k] = -666.0f;
      for(int k=0; k<3; k++) module->picked_color_min_Lab[k] =  666.0f;
      for(int k=0; k<3; k++) module->picked_color_max_Lab[k] = -666.0f;
      int box[4];
      float rgb[3], Lab[3], *in = (float *)input;
      for(int k=0; k<3; k++) Lab[k] = rgb[k] = 0.0f;
      for(int k=0; k<4; k+=2) box[k] = MIN(roi_in.width -1, MAX(0, module->color_picker_box[k]*roi_in.width));
      for(int k=1; k<4; k+=2) box[k] = MIN(roi_in.height-1, MAX(0, module->color_picker_box[k]*roi_in.height));
      const float w = 1.0/((box[3]-box[1]+1)*(box[2]-box[0]+1));
      for(int j=box[1]; j<=box[3]; j++) for(int i=box[0]; i<=box[2]; i++)
        {
          for(int k=0; k<3; k++)
          {
            module->picked_color_min[k] = fminf(module->picked_color_min[k], in[4*(roi_in.width*j + i) + k]);
            module->picked_color_max[k] = fmaxf(module->picked_color_max[k], in[4*(roi_in.width*j + i) + k]);
            rgb[k] += w*in[4*(roi_in.width*j + i) + k];
          }
          const float L = in[4*(roi_in.width*j + i) + 0];
          const float a = fminf(128, fmaxf(-128.0, in[4*(roi_in.width*j + i) + 1]*L));
          const float b = fminf(128, fmaxf(-128.0, in[4*(roi_in.width*j + i) + 2]*L));
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
      for(int k=0; k<3; k++) module->picked_color_Lab[k] = Lab[k];
      for(int k=0; k<3; k++) module->picked_color[k] = rgb[k];

      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      int needlock = !pthread_equal(pthread_self(),darktable.control->gui_thread);
      if(needlock) gdk_threads_enter();
      gtk_widget_queue_draw(module->widget);
      if(needlock) gdk_threads_leave();
    }
    else dt_pthread_mutex_unlock(&pipe->busy_mutex);

    // actual pixel processing done by module
    dt_pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown)
    {
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      return 1;
    }
    dt_times_t start;
    dt_get_times(&start);
#ifdef HAVE_OPENCL
    if(module->process_cl && piece->process_cl_ready)
    {
      // if input is not on the gpu, copy it there.
      // else, if input is on the gpu only, invalidate cpu cache line.
      // printf("for module `%s', have bufs %lX and %lX \n", module->op, (long int)cl_mem_input, (long int)*cl_mem_output);
      if(!cl_mem_input) cl_mem_input = dt_opencl_copy_host_to_device(input, roi_in.width, roi_in.height, pipe->devid, in_bpp);
      else dt_dev_pixelpipe_cache_invalidate(&(pipe->cache), input);
      *cl_mem_output = dt_opencl_alloc_device(roi_out->width, roi_out->height, pipe->devid, bpp);

      // printf("for module `%s', have bufs %d and %d bpp\n", module->op, in_bpp, bpp);
      module->process_cl(module, piece, cl_mem_input, *cl_mem_output, &roi_in, roi_out);
      clReleaseMemObject(cl_mem_input);
      // we speculate on the next plug-in to possibly copy back cl_mem_output to output,
      // so we're not just yet invalidating the (empty) output cache line.
    }
    else
    {
      // cleanup unneeded opencl buffers, and copy back to CPU buffer
      if(cl_mem_input)
      {
        dt_opencl_copy_device_to_host(input, cl_mem_input, roi_in.width, roi_in.height, pipe->devid, in_bpp);
        clReleaseMemObject(cl_mem_input);
      }
      *cl_mem_output = NULL;
#else
    {
#endif
      module->process(module, piece, input, *output, &roi_in, roi_out);

      /* process blending */
      dt_develop_blend_process(module, piece, input, *output, &roi_in, roi_out);
    }
    dt_show_times(&start, "[dev_pixelpipe]", "processing `%s' [%s]", module->name(),
                  pipe->type == DT_DEV_PIXELPIPE_PREVIEW ? "preview" : (pipe->type == DT_DEV_PIXELPIPE_FULL ? "full" : "export"));
    // in case we get this buffer from the cache, also get the processed max:
    for(int k=0; k<3; k++) piece->processed_maximum[k] = pipe->processed_maximum[k];
    dt_pthread_mutex_unlock(&pipe->busy_mutex);
    if(module == darktable.develop->gui_module)
    {
      // give the input buffer to the currently focussed plugin more weight.
      // the user is likely to change that one soon, so keep it in cache.
      dt_dev_pixelpipe_cache_reweight(&(pipe->cache), input);
    }
#ifdef _DEBUG
    dt_pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown)
    {
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      return 1;
    }
    if(strcmp(module->op, "gamma") && bpp == sizeof(float)*4) for(int k=0; k<4*roi_out->width*roi_out->height; k++)
      {
        if((k&3)<3 && !isfinite(((float*)(*output))[k]))
        {
          fprintf(stderr, "[dev_pixelpipe] module `%s' outputs non-finite floats!\n", module->name());
          break;
        }
      }
    dt_pthread_mutex_unlock(&pipe->busy_mutex);
#endif

post_process_collect_info:
    // 4) final histogram:
    dt_pthread_mutex_lock(&pipe->busy_mutex);
    if(pipe->shutdown)
    {
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      return 1;
    }
    if(dev->gui_attached && pipe == dev->preview_pipe && (strcmp(module->op, "gamma") == 0))
    {
      uint8_t *pixel = (uint8_t *)*output;
      dev->histogram_max = 0;
      memset(dev->histogram, 0, sizeof(float)*4*64);
      for(int j=0; j<roi_out->height; j+=4) for(int i=0; i<roi_out->width; i+=4)
        {
          uint8_t rgb[3];
          for(int k=0; k<3; k++)
            rgb[k] = pixel[4*j*roi_out->width+4*i+2-k]>>2;

          for(int k=0; k<3; k++)
            dev->histogram[4*rgb[k]+k] ++;
          uint8_t lum = MAX(MAX(rgb[0], rgb[1]), rgb[2]);
          dev->histogram[4*lum+3] ++;
        }
      
      for(int k=0; k<4*64; k++) dev->histogram[k] = logf(1.0 + dev->histogram[k]);
      // don't count <= 0 pixels
      for(int k=19; k<4*64; k+=4) dev->histogram_max = dev->histogram_max > dev->histogram[k] ? dev->histogram_max : dev->histogram[k];

      /* average histogram data into 4 buckets and store it to database
        this is used for image similarity histogram matching scoring. */
      dt_similarity_histogram_t avghist;
      int buckdiv = 64/DT_SIMILARITY_HISTOGRAM_BUCKETS;
      memset(&avghist,0,sizeof(dt_similarity_histogram_t));
      for(int k=0;k<64;k++)
        for(int j=0;j<4;j++) 
          avghist.rgbl[(int)(k/buckdiv)][j] = dev->histogram[k*4+j]/buckdiv;
  
      /* store the averaged histogram for future use */
      dt_similarity_store_histogram(dev->image->id, &avghist);
        
      dt_pthread_mutex_unlock(&pipe->busy_mutex);
      dt_control_queue_draw(glade_xml_get_widget (darktable.gui->main_window, "histogram"));
    }
    else dt_pthread_mutex_unlock(&pipe->busy_mutex);
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
  pipe->devid = dt_opencl_lock_device(darktable.opencl, -1);
  dt_print(DT_DEBUG_OPENCL, "[pixelpipe_process] [%s] using device %d\n", pipe->type == DT_DEV_PIXELPIPE_PREVIEW ? "preview" : (pipe->type == DT_DEV_PIXELPIPE_FULL ? "full" : "export"), pipe->devid);
  // image max is normalized before
  for(int k=0; k<3; k++) pipe->processed_maximum[k] = 1.0f; // dev->image->maximum;
  dt_iop_roi_t roi = (dt_iop_roi_t)
  {
    x, y, width, height, scale
  };
  // printf("pixelpipe homebrew process start\n");
  if(darktable.unmuted & DT_DEBUG_DEV)
    dt_dev_pixelpipe_cache_print(&pipe->cache);

  //  go through list of modules from the end:
  int pos = g_list_length(dev->iop);
  GList *modules = g_list_last(dev->iop);
  GList *pieces = g_list_last(pipe->nodes);
  void *buf = NULL;
  void *cl_mem_out = NULL;
  int out_bpp;
  if(dt_dev_pixelpipe_process_rec(pipe, dev, &buf, &cl_mem_out, &out_bpp, &roi, modules, pieces, pos))
  {
    pipe->processing = 0;
    dt_opencl_unlock_device(darktable.opencl, pipe->devid);
    pipe->devid = -1;
    return 1;
  }
  dt_pthread_mutex_lock(&pipe->backbuf_mutex);
  pipe->backbuf_hash = dt_dev_pixelpipe_cache_hash(dev->image->id, &roi, pipe, 0);
  pipe->backbuf = buf;
  pipe->backbuf_width  = width;
  pipe->backbuf_height = height;
  dt_pthread_mutex_unlock(&pipe->backbuf_mutex);

  // printf("pixelpipe homebrew process end\n");
  pipe->processing = 0;
  dt_opencl_unlock_device(darktable.opencl, pipe->devid);
  pipe->devid = -1;
  return 0;
}

void dt_dev_pixelpipe_flush_caches(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_flush(&pipe->cache);
}

void dt_dev_pixelpipe_get_dimensions(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, int width_in, int height_in, int *width, int *height)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex);
  dt_iop_roi_t roi_in = (dt_iop_roi_t)
  {
    0, 0, width_in, height_in, 1.0
  };
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
  dt_pthread_mutex_unlock(&pipe->busy_mutex);
}

