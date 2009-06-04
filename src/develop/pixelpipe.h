#ifndef DT_DEV_PIXELPIPE
#define DT_DEV_PIXELPIPE

#include <gegl.h>

/**
 * struct used by iop modules to connect to pixelpipe.
 * input and output nodes will be connected to gegl graph.
 * data can be used to store whatever private data and
 * will be freed at the end.
 */
// TODO: move to imageop.h?
// TODO: have to save params for hash if gegl has to be notified at all?
typedef struct dt_dev_pixelpipe_iop_t
{
  GeglNode *input, *output; // gegl input and output nodes
  dt_iop_module_t *module;  // the module in the dev operation stack
  void *data;               // to be used by the module for more nodes
}
dt_dev_pixelpipe_iop_t;


/**
 * this encapsulates the gegl pixel pipeline.
 * a develop module will need several of these:
 * for previews and full blits to cairo and for
 * the export function.
 */
typedef struct dt_dev_pixelpipe_t
{
  // managing gegl node
  GeglNode *gegl;
  // gegl output node (gegl:nop)
  GeglNode *output;
  // gegl input node (gegl:load-buffer)
  GeglNode *input;
  GeglBuffer *input_buffer;
  // width and height of input buffer
  int iwidth, iheight;
  // gegl instances of pixel pipeline, stored in GList of dt_dev_pixelpipe_iop_t
  GList *nodes;
}
dt_dev_pixelpipe_t;

// inits the pixelpipe with plain passthrough input/output from given buffer input and its dimensions width, height.
void dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, float *input, int width, int height);

// destroys all allocated data.
void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe);

// cleanup all gegl nodes except clean input/output
void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe);
// sync with develop_t history stack from scratch (new node added, have to pop old ones)
void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev);
// TODO: sync with develop_t history stack by just copying the top item params (same op, new params on top)
void dt_dev_pixelpipe_synch_all(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev);
// TODO: adjust gegl:nop output node according to history stack (history pop event)
void dt_dev_pixelpipe_synch_top(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev);
// process region of interest of pixels
void dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, uint8_t *output, GeglRectangle *roi, float scale);

// TODO: future application: remove/add modules from list, load from disk, user programmable etc
// TODO: add n-th module in dev list to gegl pipeline
void dt_dev_pixelpipe_add_node(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int n);
// TODO: remove n-th module from gegl pipeline
void dt_dev_pixelpipe_remove_node(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int n);

#endif
