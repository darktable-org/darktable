#ifndef DT_DEVELOP_IMAGEOP_H
#define DT_DEVELOP_IMAGEOP_H

#include "common/darktable.h"
#include "control/settings.h"
#include "develop/pixelpipe.h"
#include <gegl.h>
#include <gmodule.h>
#include <gtk/gtk.h>

#ifndef DT_USE_GEGL
  #include "develop/develop.h"
  #include "develop/iop_hsb.h"
  #define dt_red 2
  #define dt_green 1
  #define dt_blue 0
#else
struct dt_develop_t;
struct dt_dev_pixelpipe_t;
struct dt_dev_pixelpipe_iop_t;
#endif

typedef void* dt_iop_params_t;
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
  dt_iop_params_t *params;
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
  void (*gui_reset)     (struct dt_iop_module_t *self);
  void (*gui_init)      (struct dt_iop_module_t *self);
  void (*gui_cleanup)   (struct dt_iop_module_t *self);
  // TODO: add more for mouse interaction dreggn.
  void (*init) (struct dt_iop_module_t *self); // this MUST set params_size!
  void (*cleanup) (struct dt_iop_module_t *self);
  /** this inits the piece of the pipe, allocing piece->data as necessary. */
  void (*init_pipe)   (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);
  /** this resets the params to factory defaults. used at the beginning of each history synch. */
  // FIXME: should this really be named ''reset_pipe'' ??
  void (*reset_params)  (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);
  /** this commits (a mutex will be locked to synch gegl/gui) the given history params to the gegl pipe piece. */
  void (*commit_params) (struct dt_iop_module_t *self, dt_iop_params_t *params, struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);
  /** this destroys all (gegl etc) resources needed by the piece of the pipeline. */
  void (*cleanup_pipe)   (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);

#ifndef DT_USE_GEGL
  void (*execute) (float *dst, const float *src, const int32_t wd, const int32_t ht, const int32_t bufwd, const int32_t bufht,
                 dt_dev_operation_t operation, dt_dev_operation_params_t *params);
#endif
}
dt_iop_module_t;

/** loads and inits the (already alloc'ed) module. */
int dt_iop_load_module(dt_iop_module_t *module, struct dt_develop_t *dev, const char *op);
/** calls module->cleanup and closes the dl connection. */
void dt_iop_unload_module(dt_iop_module_t *module);

/** synchronized access to parameters. */
// void dt_iop_get_params(dt_iop_module_t *module, void *params);
// void dt_iop_set_params(dt_iop_module_t *module, void *params);

// TODO: replace all this shit with gegl nodes:
//  - histogram counting in small preview buf before
//  - tonecurve/gamma node
//  - ..and right at the end.
#ifndef DT_USE_GEGL
typedef struct dt_iop_t
{
  int32_t num_modules;
  dt_iop_module_t *module;
}
dt_iop_t;

// job that transforms in pixel block to out pixel block, 4x supersampling
void dt_iop_clip_and_zoom(const float *i, int32_t ix, int32_t iy, int32_t iw, int32_t ih, int32_t ibw, int32_t ibh,
                                float *o, int32_t ox, int32_t oy, int32_t ow, int32_t oh, int32_t obw, int32_t obh);

uint32_t dt_iop_create_histogram_final_f(const float *pixels, int32_t width, int32_t height, int32_t stride, uint32_t *hist, uint8_t *gamma, uint16_t *tonecurve);
uint32_t dt_iop_create_histogram_f(const float *pixels, int32_t width, int32_t height, int32_t stride, uint32_t *hist);
uint32_t dt_iop_create_histogram_8 (const uint8_t  *pixels, int32_t width, int32_t height, int32_t stride, uint32_t *hist);

void dt_iop_execute(float *dst, const float *src, const int32_t wd, const int32_t ht, const int32_t bufwd, const int32_t bufht,
                    dt_dev_operation_t operation, dt_dev_operation_params_t *params);

void dt_iop_gui_reset();
void dt_iop_gui_init();
#endif

#endif
