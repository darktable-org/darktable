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
#include <assert.h>

void dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe)
{
  pipe->changed = DT_DEV_PIPE_UNCHANGED;
  pipe->iwidth = 0;
  pipe->iheight = 0;
  pipe->nodes = NULL;
  pipe->gegl = gegl_node_new();
  pipe->input_buffer = NULL;
  pipe->input = gegl_node_new_child(pipe->gegl, "operation", "gegl:load-buffer", NULL);
  // pipe->scale = gegl_node_new_child(pipe->gegl, "operation", "gegl:scale", "filter", "nearest", "x", .5,
  // "y", .5, NULL);
  // pipe->output = gegl_node_new_child(pipe->gegl, "operation", "gegl:nop", NULL);
  pipe->output = pipe->input;
  // gegl_node_link(pipe->input, pipe->output);
  // gegl_node_link(pipe->input, pipe->scale);
  // gegl_node_link(pipe->scale, pipe->output);
  pipe->backbuf = NULL;
  pipe->backbuf_size = 0;
  pipe->processing = 0;
  pthread_mutex_init(&(pipe->backbuf_mutex), NULL);
}

void dt_dev_pixelpipe_set_input(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, float *input, int width,
                                int height)
{
  pipe->changed = DT_DEV_PIPE_UNCHANGED;
  pipe->iwidth = width;
  pipe->iheight = height;
  GeglRectangle rect = (GeglRectangle){ 0, 0, width, height };
  if(pipe->input_buffer) gegl_buffer_destroy(pipe->input_buffer);
  pipe->input_buffer = gegl_buffer_new(&rect, babl_format("RGB float"));
  gegl_buffer_set(pipe->input_buffer, NULL, babl_format("RGB float"), input, GEGL_AUTO_ROWSTRIDE);
  gegl_node_set(pipe->input, "buffer", pipe->input_buffer, NULL);
}

void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe)
{
  dt_free_align(pipe->backbuf);
  pipe->backbuf = NULL;
  pipe->backbuf_size = 0;
  pthread_mutex_destroy(&(pipe->backbuf_mutex));
  dt_dev_pixelpipe_cleanup_nodes(pipe);
  // gegl_buffer_destroy(pipe->input_buffer); // TODO: necessary?
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
  // gegl_node_link(pipe->input, pipe->output);
  // gegl_node_link(pipe->input, pipe->scale);
  // gegl_node_link(pipe->scale, pipe->output);
}

void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  // gegl_node_disconnect(pipe->output, "input");
  // gegl_node_disconnect(pipe->scale, "input");
  // for all modules in dev:
  GList *modules = dev->iop;
  GeglNode *input = pipe->input;
  // GeglNode *input = pipe->scale;
  while(modules)
  {
    // TODO: if enabled.
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    printf("connecting %s\n", module->op);
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)malloc(sizeof(dt_dev_pixelpipe_iop_t));
    piece->module = module;
    piece->data = NULL;
    piece->module->init_pipe(piece->module, pipe, piece);
    gegl_node_link(input, piece->input);
    input = piece->output;
    pipe->nodes = g_list_append(pipe->nodes, piece);
    modules = g_list_next(modules);
  }
  pipe->output = input;
  // gegl_node_link(input, pipe->scale);
  // gegl_node_link(input, pipe->output);
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
  // call reset_params on all pieces first!
  GList *nodes = pipe->nodes;
  while(nodes)
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    piece->module->commit_params(piece->module, piece->module->default_params, pipe, piece);
    nodes = g_list_next(nodes);
  }
  // go through all history items and adjust params
  GList *history = dev->history;
  for(int k = 0; k < dev->history_end; k++)
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
  switch(pipe->changed)
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

int dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int x, int y, int width, int height,
                             float scale)
{
  pipe->processing = 1;
  printf("pixelpipe process start\n");

  // have backbuf in right size:
  if(pipe->backbuf_size < width * height * 4 * sizeof(uint8_t))
  {
    pthread_mutex_lock(&pipe->backbuf_mutex);
    pipe->backbuf_size = width * height * 4 * sizeof(uint8_t);
    dt_free_align(pipe->backbuf);
    pipe->backbuf = (uint8_t *)dt_alloc_align(16, pipe->backbuf_size);
    pthread_mutex_unlock(&pipe->backbuf_mutex);
  }

  // scale node (is slow):
  // scale *= 2;
  // FIXME: this seems to be a bug in gegl. need to manually adjust updated roi here.
  GeglRectangle roi = (GeglRectangle){ x, y, width, height };
  GeglRectangle roio = (GeglRectangle){ roi.x / scale, roi.y / scale, roi.width / scale, roi.height / scale };
  roio.x = MAX(0, roio.x);
  roio.y = MAX(0, roio.y);
  roio.width = MIN(pipe->iwidth - roio.x - 1, roio.width);
  roio.height = MIN(pipe->iheight - roio.y - 1, roio.height);
  GeglProcessor *processor = gegl_node_new_processor(pipe->output, &roio);
  // gegl_node_set(pipe->scale, "x", scale, "y", scale, NULL);
  // GeglProcessor *processor = gegl_node_new_processor (pipe->output, roi);
  double progress;

  // TODO: insert constant scale node at beginning, maintain lo-res branch of pipeline (shadowed).
  // TODO: decide on scale param, which one to use.

  while(gegl_processor_work(processor, &progress))
  {
    // if history changed, abort processing?
    if(pipe->changed != DT_DEV_PIPE_UNCHANGED || dev->gui_leaving) return 1;
  }
  gegl_processor_destroy(processor);

  // gegl scale node turned out to be even slower :(
  gegl_node_blit(pipe->output, scale, &roi, babl_format("RGBA u8"), pipe->backbuf, GEGL_AUTO_ROWSTRIDE,
                 GEGL_BLIT_CACHE);
  // gegl_node_blit (pipe->output, 1.0, roi, babl_format("RGBA u8"), output, GEGL_AUTO_ROWSTRIDE,
  // GEGL_BLIT_CACHE);

  // TODO: update histograms here with this data?
  printf("pixelpipe process end\n");
  pipe->processing = 0;
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
