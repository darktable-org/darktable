#include "develop/pixelpipe.h"
#include "gui/gtk.h"
#include "control/control.h"

#include <assert.h>
#include <string.h>
#include <strings.h>
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
  pipe->iwidth = 0;
  pipe->iheight = 0;
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
  pipe->iwidth = width;
  pipe->iheight = height;
  pipe->iscale = iscale;
  pipe->input = input;
}

void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe)
{
  pipe->backbuf = NULL;
  pthread_mutex_destroy(&(pipe->backbuf_mutex));
  dt_dev_pixelpipe_cleanup_nodes(pipe);
  pipe->nodes = NULL;
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
}

void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  // for all modules in dev:
  GList *modules = dev->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    // if(module->enabled) // no! always create nodes. just don't process.
    {
      dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)malloc(sizeof(dt_dev_pixelpipe_iop_t));
      piece->enabled = module->default_enabled;
      piece->iscale = pipe->iscale;
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
    piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    if(piece->module == hist->module)
    {
      hist->module->commit_params(hist->module, hist->params, pipe, piece);
      piece->enabled = hist->enabled;
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
    piece->module->commit_params(piece->module, piece->module->default_params, pipe, piece);
    piece->enabled = piece->module->default_enabled;
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
    case DT_DEV_PIPE_UNCHANGED: case DT_DEV_PIPE_ZOOMED:
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
    int width, int height, float scale, GList *modules, GList *pieces, int pos)
{
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
      return dt_dev_pixelpipe_process_rec(pipe, dev, output, x, y, width, height, scale, g_list_previous(modules), g_list_previous(pieces), pos-1);
    }
  }

  // if available, return data
  uint64_t hash = dt_dev_pixelpipe_cache_hash(scale, x, y, dev, pos);
  if(dt_dev_pixelpipe_cache_available(&(pipe->cache), hash))
  {
    (void) dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, output);
    return 0;
  }

  // if history changed, abort processing?
  if(pipe != dev->preview_pipe && pipe->changed == DT_DEV_PIPE_ZOOMED) return 1;
  if((pipe->changed != DT_DEV_PIPE_UNCHANGED && pipe->changed != DT_DEV_PIPE_ZOOMED) || dev->gui_leaving) return 1;

  // input -> output
  if(!modules)
  { // import input array with given scale and roi
    // printf("[process] loading source image buffer\n");
    // optimized branch (for mipf-preview):
    if(scale == 1.0 && x == 0 && y == 0 && pipe->iwidth == width && pipe->iheight == height) *output = pipe->input;
    else
    {
      // printf("[process] scale/pan\n");
      // reserve new cache line: output
      (void) dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, output);
      dt_iop_clip_and_zoom(pipe->input, x/scale, y/scale, width/scale, height/scale, pipe->iwidth, pipe->iheight,
                           *output, 0, 0, width, height, width, height);
    }
  }
  else
  {
    // recurse and obtain output array in &input
    if(dt_dev_pixelpipe_process_rec(pipe, dev, &input, x, y, width, height, scale, g_list_previous(modules), g_list_previous(pieces), pos-1)) return 1;
    piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
    // reserve new cache line: output
    (void) dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, output);
    
    // tonecurve histogram (collect luminance only):
    if(pipe == dev->preview_pipe && (strcmp(module->op, "tonecurve") == 0))
    {
      float *pixel = (float *)input;
      dev->histogram_pre_max = 0;
      bzero(dev->histogram_pre, sizeof(float)*4*64);
      for(int j=0;j<height;j+=3) for(int i=0;i<width;i+=3)
      {
        // float rgb[3] = {pixel[3*j*width+3*i], pixel[3*j*width+3*i+1], pixel[3*j*width+3*i+2]};
        // uint8_t L = CLAMP(63*(0.299*rgb[0] + 0.587*rgb[1] + 0.114*rgb[2]), 0, 63);
        uint8_t L = CLAMP(63*(pixel[3*j*width+3*i]), 0, 63);
        dev->histogram_pre[4*L+3] ++;
      }
      // don't count <= 0 pixels
      for(int k=3;k<4*64;k+=4) dev->histogram_pre[k] = logf(1.0 + dev->histogram_pre[k]);
      for(int k=19;k<4*64;k+=4) dev->histogram_pre_max = dev->histogram_pre_max > dev->histogram_pre[k] ? dev->histogram_pre_max : dev->histogram_pre[k];
      dt_control_queue_draw(module->widget);
    }

    // printf("processing %s\n", module->op);
    // actual pixel processing done by module
    module->process(module, piece, input, *output, x, y, scale, width, height);

    if(strcmp(module->op, "temperature") == 0)
    {
      for(int k=0;k<width*height;k++)
      { // to YCbCr
        float rgb[3] = {((float *)*output)[3*k], ((float *)*output)[3*k+1], ((float *)*output)[3*k+2]};
        ((float *)*output)[3*k+0] =  0.299*rgb[0] + 0.587*rgb[1] + 0.114*rgb[2];
        ((float *)*output)[3*k+1] = -0.147*rgb[0] - 0.289*rgb[1] + 0.437*rgb[2];
        ((float *)*output)[3*k+2] =  0.615*rgb[0] - 0.515*rgb[1] - 0.100*rgb[2];
      }
    }
#if 0
    const float m[9] = {0.299,  0.587, 0.114,
                        -0.147, -0.289, 0.437,
                         0.615, -0.515, -0.100};
    float inv[9];
#define A(y, x) m[(y - 1) * 3 + (x - 1)]
#define B(y, x) inv[(y - 1) * 3 + (x - 1)]
    const float det =
      A(1, 1) * (A(3, 3) * A(2, 2) - A(3, 2) * A(2, 3)) -
      A(2, 1) * (A(3, 3) * A(1, 2) - A(3, 2) * A(1, 3)) +
      A(3, 1) * (A(2, 3) * A(1, 2) - A(2, 2) * A(1, 3));

    const float invDet = 1.f / det;
    B(1, 1) =  invDet * (A(3, 3) * A(2, 2) - A(3, 2) * A(2, 3));
    B(1, 2) = -invDet * (A(3, 3) * A(1, 2) - A(3, 2) * A(1, 3));
    B(1, 3) =  invDet * (A(2, 3) * A(1, 2) - A(2, 2) * A(1, 3));

    B(2, 1) = -invDet * (A(3, 3) * A(2, 1) - A(3, 1) * A(2, 3));
    B(2, 2) =  invDet * (A(3, 3) * A(1, 1) - A(3, 1) * A(1, 3));
    B(2, 3) = -invDet * (A(2, 3) * A(1, 1) - A(2, 1) * A(1, 3));

    B(3, 1) =  invDet * (A(3, 2) * A(2, 1) - A(3, 1) * A(2, 2));
    B(3, 2) = -invDet * (A(3, 2) * A(1, 1) - A(3, 1) * A(1, 2));
    B(3, 3) =  invDet * (A(2, 2) * A(1, 1) - A(2, 1) * A(1, 2));
#undef A
#undef B
    printf("const float inv[9] = {%f, %f, %f,\n", inv[0], inv[1], inv[2]);
    printf("     %f, %f, %f,\n", inv[3], inv[4], inv[5]);
    printf("     %f, %f, %f};\n", inv[6], inv[7], inv[8]);
    exit(0);
#endif


    // if(strcmp(module->op, "colorcorrection") == 0)
    if(strcmp(module->op, "tonecurve") == 0)
    {
      for(int k=0;k<width*height;k++)
      { // to RGB 16
        float YCbCr[3] = {((float *)*output)[3*k], ((float *)*output)[3*k+1], ((float *)*output)[3*k+2]};
        // ((float *)*output)[3*k+0] = YCbCr[0]                  + 1.140*YCbCr[2];
        // ((float *)*output)[3*k+1] = YCbCr[0] - 0.394*YCbCr[1] - 0.581*YCbCr[2];
        // ((float *)*output)[3*k+2] = YCbCr[0] + 2.028*YCbCr[1]                 ;
        ((uint16_t *)*output)[3*k+0] = CLAMP((int)(0xfffful*(YCbCr[0]                  + 1.140*YCbCr[2])), 0, 0xffff);
        ((uint16_t *)*output)[3*k+1] = CLAMP((int)(0xfffful*(YCbCr[0] - 0.394*YCbCr[1] - 0.581*YCbCr[2])), 0, 0xffff);
        ((uint16_t *)*output)[3*k+2] = CLAMP((int)(0xfffful*(YCbCr[0] + 2.028*YCbCr[1]                 )), 0, 0xffff);
      }
    }

    // final histogram:
    if(pipe == dev->preview_pipe && (strcmp(module->op, "gamma") == 0))
    {
      uint8_t *pixel = (uint8_t *)*output;
      dev->histogram_max = 0;
      bzero(dev->histogram, sizeof(float)*4*64);
      for(int j=0;j<height;j+=4) for(int i=0;i<width;i+=4)
      {
        uint8_t rgb[3];
        for(int k=0;k<3;k++)
          rgb[k] = pixel[4*j*width+4*i+2-k]>>2;

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
  // printf("pixelpipe homebrew process start\n");

  //  go through list of modules from the end:
  int pos = g_list_length(dev->iop);
  GList *modules = g_list_last(dev->iop);
  GList *pieces = g_list_last(pipe->nodes);
  void *buf = NULL;
  if(dt_dev_pixelpipe_process_rec(pipe, dev, &buf, x, y, width, height, scale, modules, pieces, pos)) return 1;
  pthread_mutex_lock(&pipe->backbuf_mutex);
  pipe->backbuf = buf;
  pthread_mutex_unlock(&pipe->backbuf_mutex);

  // printf("pixelpipe homebrew process end\n");
  pipe->processing = 0;
  return 0;
}

void dt_dev_pixelpipe_flush_caches(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_flush(&pipe->cache);
}

