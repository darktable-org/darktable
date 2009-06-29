#ifndef DT_DEVELOP_IMAGEOP_H
#define DT_DEVELOP_IMAGEOP_H

#include "common/darktable.h"
#include "control/settings.h"
#include "develop/pixelpipe.h"
#include <gegl.h>
#include <gmodule.h>
#include <gtk/gtk.h>

struct dt_develop_t;
struct dt_dev_pixelpipe_t;
struct dt_dev_pixelpipe_iop_t;

typedef struct dt_iop_params_t
{
  int keep;
}
dt_iop_params_t;
typedef void* dt_iop_gui_data_t;
typedef void* dt_iop_data_t;

struct dt_iop_module_t;
typedef struct dt_iop_module_t
{
  /** opened module. */
  GModule *module;
  /** used to identify this module in the history stack. */
  int32_t instance;
  /** reference for dlopened libs. */
  darktable_t *dt;
  /** the module is used in this develop module. */
  struct dt_develop_t *dev;
  /** non zero if this node should be processed. */
  int32_t enabled;
  /** parameters for the operation. will be replaced by history revert. */
  dt_iop_params_t *params, *default_params;
  /** exclusive access to params is needed, as gui and gegl processing is async. */
  pthread_mutex_t params_mutex;
  /** size of individual params struct. */
  int32_t params_size;
  /** parameters needed if a gui is attached. will be NULL if in export/batch mode. */
  dt_iop_gui_data_t *gui_data;
  // other stuff that may be needed by the module (as GeglNode*), not only in gui mode.
  // dt_iop_data_t *data;
  /** string identifying this operation. */
  dt_dev_operation_t op;
  /** child widget which is added to the GtkExpander. */
  GtkWidget *widget;
  /** callback methods for gui. */
  /** synch gtk interface with gui params, if necessary. */
  void (*gui_update)    (struct dt_iop_module_t *self);
  /** construct widget. */
  void (*gui_init)      (struct dt_iop_module_t *self);
  /** destroy widget. */
  void (*gui_cleanup)   (struct dt_iop_module_t *self);

  // TODO: add more for mouse interaction dreggn.
  
  void (*init) (struct dt_iop_module_t *self); // this MUST set params_size!
  void (*cleanup) (struct dt_iop_module_t *self);
  /** this inits the piece of the pipe, allocing piece->data as necessary. */
  void (*init_pipe)   (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);
  /** this resets the params to factory defaults. used at the beginning of each history synch. */
  /** this commits (a mutex will be locked to synch gegl/gui) the given history params to the gegl pipe piece. */
  void (*commit_params) (struct dt_iop_module_t *self, struct dt_iop_params_t *params, struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);
  /** this destroys all (gegl etc) resources needed by the piece of the pipeline. */
  void (*cleanup_pipe)   (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);

  /** this is the temp homebrew callback to operations, as long as gegl is so slow.
    * x,y, and scale are just given for orientation in the framebuffer. i and o are
    * scaled to the same size width*height and contain a max of 3 floats. other color
    * formats may be filled by this callback, if the pipeline can handle it. */
  void (*process) (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i, void *o, int x, int y, float scale, int width, int height);
}
dt_iop_module_t;

/** loads and inits the (already alloc'ed) module. */
int dt_iop_load_module(dt_iop_module_t *module, struct dt_develop_t *dev, const char *op);
/** calls module->cleanup and closes the dl connection. */
void dt_iop_unload_module(dt_iop_module_t *module);

/** for homebrew pixel pipe: zoom pixel array. */
void dt_iop_clip_and_zoom(const float *i, int32_t ix, int32_t iy, int32_t iw, int32_t ih, int32_t ibw, int32_t ibh,
                                float *o, int32_t ox, int32_t oy, int32_t ow, int32_t oh, int32_t obw, int32_t obh);
/** for homebrew pixel pipe: convert sRGB to Lab, float buffers. */
void dt_iop_sRGB_to_Lab(const float *in, float *out, int x, int y, float scale, int width, int height);
/** for homebrew pixel pipe: convert Lab to sRGB, both uint16_t buffers. */
void dt_iop_Lab_to_sRGB_16(uint16_t *in, uint16_t *out, int x, int y, float scale, int width, int height);
/** for homebrew pixel pipe: convert Lab to sRGB, float buffers. */
void dt_iop_Lab_to_sRGB(const float *in, float *out, int x, int y, float scale, int width, int height);


#endif
