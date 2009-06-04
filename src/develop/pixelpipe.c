#include "develop/pixelpipe.h"

void dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, float *input, int width, int height)
{
  pipe->scale = 1.0;
  pipe->x = pipe->y = 0;
  pipe->width = 1;
  pipe->height = 1;

  pipe->gegl = gegl_node_new();
  GeglRectangle rect = (GeglRectangle){0, 0, width, height};
  pipe->input_buffer = gegl_buffer_new(&rect, babl_format("RGB float"));
  pipe->input = gegl_node_new_child(pipe->gegl, "operation", "gegl:load-buffer", "buffer", pipe->input_buffer, NULL);
  pipe->output = gegl_node_new_child(pipe->gegl, "operation", "gegl:nop", NULL);
  gegl_node_link(pipe->input, pipe->output);
}

void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe);
void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe);
void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev);
void dt_dev_pixelpipe_synch_nodes(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev);
void dt_dev_pixelpipe_set_top(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev);

void dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, uint8_t *output)
{
  // FIXME: quick hack: fill output buffer and apply gamma after that.
  // TODO: copy most of dt_dev_process_preview_job
}
