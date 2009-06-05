#include "develop/pixelpipe.h"
#include <assert.h>

void dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, float *input, int width, int height)
{
  pipe->iwidth = width;
  pipe->iheight = height;
  pipe->nodes = NULL;
  pipe->gegl = gegl_node_new();
  GeglRectangle rect = (GeglRectangle){0, 0, width, height};
  pipe->input_buffer = gegl_buffer_new(&rect, babl_format("RGB float"));
  gegl_buffer_set(pipe->input_buffer, NULL, babl_format("RGB float"), input, GEGL_AUTO_ROWSTRIDE);
  pipe->input = gegl_node_new_child(pipe->gegl, "operation", "gegl:load-buffer", "buffer", pipe->input_buffer, NULL);
  // TODO: replace by custom gegl:lin_gamma
  pipe->output = gegl_node_new_child(pipe->gegl, "operation", "gegl:nop", NULL);
  gegl_node_link(pipe->input, pipe->output);
}

void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cleanup_nodes(pipe);
  gegl_buffer_destroy(pipe->input_buffer); // TODO: necessary?
  g_object_unref(pipe->gegl); // should destroy all gegl related stuff.
  pipe->gegl = NULL;
  pipe->input = NULL;
  pipe->output = NULL;
  pipe->input_buffer = NULL;
  pipe->nodes = NULL;
}

void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe)
{
  // gegl unlink and destroy all nodes except in/out:
  GList *nodes = pipe->nodes;
  while(nodes)
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    piece->module->cleanup_pipe(piece->module, pipe, piece);
    nodes = g_list_next(nodes);
  }
  gegl_node_link(pipe->input, pipe->output);
}

void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  gegl_node_disconnect(pipe->output, "input");
  // for all modules in dev:
  GList *modules = dev->iop;
  GeglNode *input = pipe->input;
  while(modules)
  {
    // TODO: if enabled.
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    printf("connecting %s\n", module->op);
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)malloc(sizeof(dt_dev_pixelpipe_iop_t));
    piece->module = module;
    piece->data = NULL;
    // FIXME: is this a race condition gui/gegl on piece->module->params?
    piece->module->init_pipe(piece->module, piece->module->params, pipe, piece);
    gegl_node_link(input, piece->input);
    input = piece->output;
    pipe->nodes = g_list_append(pipe->nodes, piece);
    modules = g_list_next(modules);
  }
  gegl_node_link(input, pipe->output);
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
  pthread_mutex_lock(&dev->history_mutex);
  // go through all history items and adjust params
  GList *history = dev->history;
  for(int k=0;k<dev->history_end;k++)
  {
    dt_dev_pixelpipe_synch(pipe, dev, history);
    history = g_list_next(history);
  }
  pthread_mutex_unlock(&dev->history_mutex);
}

void dt_dev_pixelpipe_synch_top(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  pthread_mutex_lock(&dev->history_mutex);
  GList *history = g_list_nth(dev->history, dev->history_end - 1);
  if(history) dt_dev_pixelpipe_synch(pipe, dev, history);
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

void dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, uint8_t *output, GeglRectangle *roi, float scale)
{
  // FIXME: quick hack: fill output buffer and apply gamma after that.
  GeglRectangle roio = (GeglRectangle){roi->x/scale, roi->y/scale, roi->width/scale, roi->height/scale};
  roio.x      = MAX(0, roio.x);
  roio.y      = MAX(0, roio.y);
  roio.width  = MIN(pipe->iwidth -roio.x-1, roio.width);
  roio.height = MIN(pipe->iheight-roio.y-1, roio.height);
  GeglProcessor *processor = gegl_node_new_processor (pipe->output, &roio);
  double         progress;

  while (gegl_processor_work (processor, &progress))
  {
    // if history changed, abort processing?
    // if(dev->history_changed) return;
  }
  gegl_processor_destroy (processor);

  gegl_node_blit (pipe->output, scale, roi, babl_format("RGBA u8"), output, GEGL_AUTO_ROWSTRIDE, GEGL_BLIT_CACHE);

  // TODO: update histograms here with this data?
  // TODO: implement lin/gamma gegl node and directly draw to gtk pixbuf!
  for(int i=0;i<roi->width*roi->height;i++)
  {
    uint8_t tmp[3] = {output[4*i+2], output[4*i+1], output[4*i]};
    for(int k=0;k<3;k++) output[4*i+k] = dev->gamma[tmp[k]];
  }
}

