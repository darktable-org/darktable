#include "develop/pixelpipe.h"
#include "gui/gtk.h"
#include "control/control.h"

#include <assert.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>

// this is to ensure compatibility with pixelpipe_gegl.c, which does not need to build the other module:
#include "develop/pixelpipe_cache.c"

void dt_dev_pixelpipe_init_export(dt_dev_pixelpipe_t *pipe, int32_t width, int32_t height)
{
  dt_dev_pixelpipe_init_cached(pipe, 3*sizeof(float)*width*height, 1);
}

void dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe)
{
  // TODO: this is definitely a waste of memory (165 MB for the screen cache for both pipes) :)
  dt_dev_pixelpipe_init_cached(pipe, 3*sizeof(float)*DT_IMAGE_WINDOW_SIZE*DT_IMAGE_WINDOW_SIZE, 5);
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
  pipe->input_timestamp = 0;
  pthread_mutex_init(&(pipe->backbuf_mutex), NULL);
}

void dt_dev_pixelpipe_set_input(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, float *input, int width, int height, float iscale)
{
  pipe->iwidth  = width;
  pipe->iheight = height;
  pipe->iscale = iscale;
  pipe->input = input;
}

void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe)
{
  pipe->backbuf = NULL;
  pthread_mutex_destroy(&(pipe->backbuf_mutex));
  dt_dev_pixelpipe_cleanup_nodes(pipe);
  dt_dev_pixelpipe_cache_cleanup(&(pipe->cache));
}

void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe)
{
  // destroy all nodes
  GList *nodes = pipe->nodes;
  while(nodes)
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    piece->module->cleanup_pipe(piece->module, pipe, piece);
    nodes = g_list_next(nodes);
  }
  pipe->nodes = NULL;
}

void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  assert(pipe->nodes == NULL);
  // for all modules in dev:
  GList *modules = dev->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    // if(module->enabled) // no! always create nodes. just don't process.
    {
      dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)malloc(sizeof(dt_dev_pixelpipe_iop_t));
      piece->enabled = module->enabled;
      piece->iscale = pipe->iscale;
      piece->module = module;
      piece->data = NULL;
      piece->hash = 0;
      piece->module->init_pipe(piece->module, pipe, piece);
      pipe->nodes = g_list_append(pipe->nodes, piece);
    }
    modules = g_list_next(modules);
  }
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
}

void dt_dev_pixelpipe_synch_top(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  GList *history = g_list_nth(dev->history, dev->history_end - 1);
  if(history) dt_dev_pixelpipe_synch(pipe, dev, history);
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
  dt_iop_roi_t roi_in;

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
      module->modify_roi_in(module, piece, roi_out, &roi_in);
      return dt_dev_pixelpipe_process_rec(pipe, dev, output, &roi_in, g_list_previous(modules), g_list_previous(pieces), pos-1);
    }
  }

  // if available, return data
  uint64_t hash = dt_dev_pixelpipe_cache_hash(dev->image->id, roi_out->scale, roi_out->x, roi_out->y, pipe, pos);
  if(dt_dev_pixelpipe_cache_available(&(pipe->cache), hash))
  {
    // if(module) printf("found valid buf pos %d in cache for module %s %s %lu\n", pos, module->op, pipe == dev->preview_pipe ? "[preview]" : "", hash);
    if(pos == 0) (void) dt_dev_pixelpipe_cache_get_important(&(pipe->cache), hash, output);
    else         (void) dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, output);
    return 0;
  }

  // if history changed or exit event, abort processing?
  // preview pipe: abort on all but zoom events (same buffer anyways)
  if(pipe != dev->preview_pipe && pipe->changed == DT_DEV_PIPE_ZOOMED) return 1;
  if((pipe->changed != DT_DEV_PIPE_UNCHANGED && pipe->changed != DT_DEV_PIPE_ZOOMED) || dev->gui_leaving) return 1;

  // input -> output
  if(!modules)
  { // import input array with given scale and roi
    // printf("[process] loading source image buffer\n");
    // optimized branch (for mipf-preview):
    if(roi_out->scale == 1.0 && roi_out->x == 0 && roi_out->y == 0 && pipe->iwidth == roi_out->width && pipe->iheight == roi_out->height) *output = pipe->input;
    else
    {
      // printf("[process] scale/pan\n");
      // reserve new cache line: output
      (void) dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, output);
      dt_iop_clip_and_zoom(pipe->input, roi_out->x/roi_out->scale, roi_out->y/roi_out->scale, roi_out->width/roi_out->scale, roi_out->height/roi_out->scale, pipe->iwidth, pipe->iheight,
                           *output, 0, 0, roi_out->width, roi_out->height, roi_out->width, roi_out->height);
    }
  }
  else
  {
    // recurse and obtain output array in &input
    module->modify_roi_in(module, piece, roi_out, &roi_in);
    if(dt_dev_pixelpipe_process_rec(pipe, dev, &input, &roi_in, g_list_previous(modules), g_list_previous(pieces), pos-1)) return 1;
    piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
    // reserve new cache line: output
    // if(module) printf("reserving new buf in cache for module %s %s\n", module->op, pipe == dev->preview_pipe ? "[preview]" : "");
    if(pos == 0) (void) dt_dev_pixelpipe_cache_get_important(&(pipe->cache), hash, output);
    else         (void) dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, output);
    
    // tonecurve histogram (collect luminance only):
    if(pipe == dev->preview_pipe && (strcmp(module->op, "tonecurve") == 0))
    {
      float *pixel = (float *)input;
      dev->histogram_pre_max = 0;
      bzero(dev->histogram_pre, sizeof(float)*4*64);
      for(int j=0;j<roi_out->height;j+=3) for(int i=0;i<roi_out->width;i+=3)
      {
        uint8_t L = CLAMP(63/100.0*(pixel[3*j*roi_out->width+3*i]), 0, 63);
        dev->histogram_pre[4*L+3] ++;
      }
      // don't count <= 0 pixels
      for(int k=3;k<4*64;k+=4) dev->histogram_pre[k] = logf(1.0 + dev->histogram_pre[k]);
      for(int k=19;k<4*64;k+=4) dev->histogram_pre_max = dev->histogram_pre_max > dev->histogram_pre[k] ? dev->histogram_pre_max : dev->histogram_pre[k];
      dt_control_queue_draw(module->widget);
    }

    // printf("processing %s\n", module->op);
    // actual pixel processing done by module
    module->process(module, piece, input, *output, roi_out->x, roi_out->y, roi_out->scale, roi_out->width, roi_out->height);

    if(strcmp(module->op, "colorout") == 0)
      for(int k=0;k<3*roi_out->width*roi_out->height;k++) ((uint16_t *)*output)[k] = CLAMP((int)(0xfffful*(((float *)*output))[k]), 0, 0xffff);

    // final histogram:
    if(pipe == dev->preview_pipe && (strcmp(module->op, "gamma") == 0))
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
      dt_control_queue_draw(glade_xml_get_widget (darktable.gui->main_window, "histogram"));
    }
  }
  return 0;
}


int dt_dev_pixelpipe_process_16(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int x, int y, int width, int height, float scale)
{
  // temporarily disable gamma mapping.
  GList *gammap = g_list_last(pipe->nodes);
  dt_dev_pixelpipe_iop_t *gamma = (dt_dev_pixelpipe_iop_t *)gammap->data;
  gamma->enabled = 0;
  int ret = dt_dev_pixelpipe_process(pipe, dev, x, y, width, height, scale);
  gamma->enabled = 1;
  return ret;
}

int dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int x, int y, int width, int height, float scale)
{
  pipe->processing = 1;
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
      module->modify_roi_out(module, piece, &roi_out, &roi_in);
      roi_in = roi_out;
    }
    modules = g_list_next(modules);
    pieces = g_list_next(pieces);
  }
  *width  = roi_out.width;
  *height = roi_out.height;
}
