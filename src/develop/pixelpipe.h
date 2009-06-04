#ifndef DT_DEV_PIXELPIPE
#define DT_DEV_PIXELPIPE

#include <gegl.h>

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
  // gegl instances of pixel pipeline
  GList *nodes;
  // scale, region of interest
  float scale;
  int x, y, width, height;
}
dt_dev_pixelpipe_t;

// inits the pixelpipe with plain passthrough input/output from given buffer input and its dimensions width, height.
void dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, float *input, int width, int height);

// destroys all allocated data.
void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe);
// TODO: interface:
// TODO: cleanup
void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe);
// TODO: sync with develop_t history stack from scratch (new node added, have to pop old ones)
void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev);
// TODO: sync with develop_t history stack by just copying the top item params (same op, new params on top)
void dt_dev_pixelpipe_synch_nodes(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev);
// TODO: adjust gegl:nop output node according to history stack (history pop event)
void dt_dev_pixelpipe_set_top(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev);
// TODO: process pixel
void dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, uint8_t *output);

#endif
