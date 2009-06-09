#ifndef DT_DEV_PIXELPIPE
#define DT_DEV_PIXELPIPE

#include "develop/imageop.h"
#include "develop/develop.h"
#include "develop/pixelpipe_cache.h"

/**
 * struct used by iop modules to connect to pixelpipe.
 * input and output nodes will be connected to gegl graph.
 * data can be used to store whatever private data and
 * will be freed at the end.
 */
struct dt_iop_module_t;
typedef struct dt_dev_pixelpipe_iop_t
{
  struct dt_iop_module_t *module;  // the module in the dev operation stack
  void *data;                      // to be used by the module to store stuff per pipe piece
}
dt_dev_pixelpipe_iop_t;

typedef enum dt_dev_pixelpipe_change_t
{
  DT_DEV_PIPE_UNCHANGED   = 0,  // no event
  DT_DEV_PIPE_TOP_CHANGED = 1,  // only params of top element changed
  DT_DEV_PIPE_REMOVE      = 2,  // possibly elements of the pipe have to be removed
  DT_DEV_PIPE_SYNCH       = 3   // all nodes up to end need to be synched, but no removal of module pieces is necessary
}
dt_dev_pixelpipe_change_t;

/**
 * this encapsulates the gegl pixel pipeline.
 * a develop module will need several of these:
 * for previews and full blits to cairo and for
 * the export function.
 */
typedef struct dt_dev_pixelpipe_t
{
  // store history/zoom caches
  dt_dev_pixelpipe_cache_t cache;
  // input buffer
  float *input;
  // width and height of input buffer
  int iwidth, iheight;
  // gegl instances of pixel pipeline, stored in GList of dt_dev_pixelpipe_iop_t
  GList *nodes;
  // event flag
  dt_dev_pixelpipe_change_t changed;
}
dt_dev_pixelpipe_t;

struct dt_develop_t;

// inits the pixelpipe with plain passthrough input/output and empty input
void dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe);
// constructs a new input gegl_buffer from given RGB float array
void dt_dev_pixelpipe_set_input(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, float *input, int width, int height);

// destroys all allocated data.
void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe);

// wrapper for cleanup_nodes, create_nodes, synch_all and synch_top, decides upon changed event which one to take on. also locks dev->history_mutex.
void dt_dev_pixelpipe_change(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);
// cleanup all gegl nodes except clean input/output
void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe);
// sync with develop_t history stack from scratch (new node added, have to pop old ones)
void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);
// sync with develop_t history stack by just copying the top item params (same op, new params on top)
void dt_dev_pixelpipe_synch_all(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);
// adjust gegl:nop output node according to history stack (history pop event)
void dt_dev_pixelpipe_synch_top(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);

// process region of interest of pixels. returns 1 if pipe was altered during processing.
int dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, uint8_t *output, GeglRectangle *roi, float scale);

// TODO: future application: remove/add modules from list, load from disk, user programmable etc
// TODO: add n-th module in dev list to gegl pipeline
void dt_dev_pixelpipe_add_node(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, int n);
// TODO: remove n-th module from gegl pipeline
void dt_dev_pixelpipe_remove_node(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, int n);

#endif
