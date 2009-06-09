#include "develop/pixelpipe.h"
#include <assert.h>

// this is to ensure compatibility with pixelpipe_gegl.c, which does not need to build the other module:
#include "develp/pixelpipe_cache.c"

void dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe)
{
  pipe->changed = DT_DEV_PIPE_UNCHANGED;
  pipe->iwidth = 0;
  pipe->iheight = 0;
  pipe->nodes = NULL;
  pipe->backbuf_size = 3*sizeof(float)*
  dt_dev_pixelpipe_cache_init(&(pipe.cache), 5, );
  pipe->backbuf = NULL;
  pipe->processing = 0;
  pthread_mutex_init(&(pipe->backbuf_mutex), NULL);
}

void dt_dev_pixelpipe_set_input(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, float *input, int width, int height)
{
  pipe->changed = DT_DEV_PIPE_UNCHANGED;
  pipe->iwidth = width;
  pipe->iheight = height;
  pipe->input = input;
}

void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe)
{
  pipe->backbuf = NULL;
  pthread_mutex_destroy(&(pipe->backbuf_mutex));
  dt_dev_pixelpipe_cleanup_nodes(pipe);
  pipe->nodes = NULL;
  dt_dev_pixelpipe_cache_cleanup(&(pipe.cache));
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
}

void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  // for all modules in dev:
  GList *modules = dev->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    if(module->enabled)
    {
      printf("connecting %s\n", module->op);
      dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)malloc(sizeof(dt_dev_pixelpipe_iop_t));
      piece->module = module;
      piece->data = NULL;
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
    // TODO: have to store params in pixelpipe_iop as hash to avoid update?
    piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    if(piece->module == hist->module) hist->module->commit_params(hist->module, hist->params, pipe, piece);
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
    piece->module->reset_params(piece->module, pipe, piece);
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
  switch (pipe->changed)
  {
    case DT_DEV_PIPE_UNCHANGED:
      break;
    case DT_DEV_PIPE_TOP_CHANGED:
      // only top history item changed.
      dt_dev_pixelpipe_synch_top(pipe, dev);
      break;
    case DT_DEV_PIPE_SYNCH:
      // pipeline topology remains intact, only change all params.
      dt_dev_pixelpipe_synch_all(pipe, dev);
      break;
    default: // DT_DEV_PIPE_REMOVE
      // modules have been added in between or removed. need to rebuild the whole pipeline.
      dt_dev_pixelpipe_cleanup_nodes(pipe);
      dt_dev_pixelpipe_create_nodes(pipe, dev);
      break;
  }
  pipe->changed = DT_DEV_PIPE_UNCHANGED;
  pthread_mutex_unlock(&dev->history_mutex);
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
int dt_dev_pixelpipe_process_rec(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, void **output, int x, int y,
    int width, int height, float scale, GList *modules, int pos)
{
  void *input = NULL;
  dt_iop_module_t *module = NULL;
  if(modules)
  {
    module = (dt_iop_module_t *)modules->data;
    // skip this module?
    if(!module->enabled)
      return dt_dev_pixelpipe_process_rec(pipe, dev, &output, x, y, width, height, scale, g_list_previous(modules), pos-1);
  }

  // if available, return data
  uint64_t hash = dt_dev_pixelpipe_cache_hash(scale, x, y, dev, pos);
  if(dt_dev_pixelpipe_cache_available(&(pipe.cache), hash))
  {
    (void) dt_dev_pixelpipe_cache_get(&(pipe.cache), hash, output);
    return 0;
  }

  // if history changed, abort processing?
  if(pipe->changed != DT_DEV_PIPE_UNCHANGED || dev->gui_leaving) return 1;

  // input -> output
  if(!modules)
  { // import input array with given scale and roi
    // optimized branch (for mipf-preview):
    if(scale == 1.0 && x == 0 && y == 0 && pipe->iwidth == width && pipe->iheight == height) *output = pipe->input;
    else
    {
      // reserve new cache line: output
      (void) dt_dev_pixelpipe_cache_get(&(pipe.cache), hash, output);
      dt_iop_clip_and_zoom(pipe->input, x/scale, y/scale, width/scale, height/scale, pipe->iwidth, pipe->iheight,
                           *output, x, y, width, height, width, height);
    }
  }
  else
  {
    // recurse and obtain output array in &input
    if(dt_dev_pixelpipe_process_rec(pipe, dev, &input, x, y, width, height, scale, g_list_previous(modules), pos-1)) return 1;
    // reserve new cache line: output
    (void) dt_dev_pixelpipe_cache_get(&(pipe.cache), hash, output);
    // TODO: pixel processing using module->process!
    return 0;
  }
}

int dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int x, int y, int width, int height, float scale)
{
  pipe->processing = 1;
  printf("pixelpipe homebrew process start\n");

  //  go through list of modules from the end:
  int pos = g_list_length(dev->iop);
  GList *modules = g_list_last(dev->iop);
  void *buf = NULL;
  int ret = dt_dev_pixelpipe_process_rec(pipe, dev, &buf, x, y, width, height, scale, modules, pos);
  pthread_mutex_lock(&pipe->backbuf_mutex);
  pipe->backbuf = buf;
  pthread_mutex_unlock(&pipe->backbuf_mutex);

  // TODO: update histograms here with this data?
  printf("pixelpipe homebrew process end\n");
  pipe->processing = 0;
  return 0;
}

