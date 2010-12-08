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
#ifndef DT_DEVELOP_IMAGEOP_H
#define DT_DEVELOP_IMAGEOP_H

#include "common/darktable.h"
#include "control/settings.h"
#include "develop/pixelpipe.h"
#include "dtgtk/togglebutton.h"
#include <gmodule.h>
#include <gtk/gtk.h>
#include <sched.h>
struct dt_develop_t;
struct dt_dev_pixelpipe_t;
struct dt_dev_pixelpipe_iop_t;
struct dt_iop_roi_t;

#define	IOP_GROUP_BASIC    1
#define	IOP_GROUP_COLOR    2
#define	IOP_GROUP_CORRECT  4
#define	IOP_GROUP_EFFECT   8

#define	IOP_GROUP_ALL (IOP_GROUP_BASIC|IOP_GROUP_COLOR|IOP_GROUP_CORRECT|IOP_GROUP_EFFECT)

/** Flag for the iop module to be enabled/included by default when creating a style */
#define	IOP_FLAGS_INCLUDE_IN_STYLES	1

typedef struct dt_iop_params_t
{
  int keep;
}
dt_iop_params_t;
typedef void dt_iop_gui_data_t;
typedef void dt_iop_data_t;
typedef void dt_iop_global_data_t;

struct dt_iop_module_t;
typedef struct dt_iop_module_t
{
  /** opened module. */
  GModule *module;
  /** used to identify this module in the history stack. */
  int32_t instance;
  /** order in which plugins are stacked. */
  int32_t priority;
  /** module sets this if the enable checkbox should be hidden. */
  int32_t hide_enable_button;
  /** set to 1 if you want an input color picked during next eval. gui mode only. */
  int32_t request_color_pick;
  /** bounding box in which the mean color is requested. */
  float color_picker_box[4];
  /** place to store the picked color. */
  float picked_color[3], picked_color_min[3], picked_color_max[3];
  float picked_color_Lab[3], picked_color_min_Lab[3], picked_color_max_Lab[3];
  /** reference for dlopened libs. */
  darktable_t *dt;
  /** the module is used in this develop module. */
  struct dt_develop_t *dev;
  /** non zero if this node should be processed. */
  int32_t enabled, default_enabled, factory_enabled;
  /** parameters for the operation. will be replaced by history revert. */
  dt_iop_params_t *params, *default_params, *factory_params;
  /** exclusive access to params is needed, as gui and gegl processing is async. */
  pthread_mutex_t params_mutex;
  /** size of individual params struct. */
  int32_t params_size;
  /** parameters needed if a gui is attached. will be NULL if in export/batch mode. */
  dt_iop_gui_data_t *gui_data;
  /** other stuff that may be needed by the module, not only in gui mode. */
  dt_iop_global_data_t *data;
  /** string identifying this operation. */
  dt_dev_operation_t op;
  /** child widget which is added to the GtkExpander. */
  GtkWidget *widget;
  /** off button, somewhere in header, common to all plug-ins. */
  GtkDarktableToggleButton *off;
  /** this widget contains all of the module: expander and label decoration. */
  GtkWidget *topwidget;
  /** button used to show/hide this module in the plugin list. */
  GtkWidget *showhide;
  /** expander containing the widget. */
  GtkExpander *expander;

  /** version of the parameters in the database. */
  int (*version)          ();
  /** get name of the module, to be translated. */
  const char* (*name)     ();
  /** get the groups this module belongs to. */
  int (*groups)           ();
  /** get the iop module flags. */
  int (*flags)            ();
  /** how many bytes per pixel in the output. */
  int (*output_bpp)       (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);
  
  /** callback methods for gui. */
  /** synch gtk interface with gui params, if necessary. */
  void (*gui_update)      (struct dt_iop_module_t *self);
  /** construct widget. */
  void (*gui_init)        (struct dt_iop_module_t *self);
  /** destroy widget. */
  void (*gui_cleanup)     (struct dt_iop_module_t *self);
  /** optional method called after darkroom expose. */
  void (*gui_post_expose) (struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery);

  /** optional event callbacks */
  int  (*mouse_leave)     (struct dt_iop_module_t *self);
  int  (*mouse_moved)     (struct dt_iop_module_t *self, double x, double y, int which);
  int  (*button_released) (struct dt_iop_module_t *self, double x, double y, int which, uint32_t state);
  int  (*button_pressed)  (struct dt_iop_module_t *self, double x, double y, int which, int type, uint32_t state);
  int  (*key_pressed)     (struct dt_iop_module_t *self, uint16_t which);
  int  (*scrolled)        (struct dt_iop_module_t *self, double x, double y, int up);
  void (*configure)       (struct dt_iop_module_t *self, int width, int height);
  
  void (*init)            (struct dt_iop_module_t *self); // this MUST set params_size!
  void (*cleanup)         (struct dt_iop_module_t *self);
  /** this initializes static, hardcoded presets for this module and is called only once per run of dt. */
  void (*init_presets)    (struct dt_iop_module_t *self);
  /** this inits the piece of the pipe, allocing piece->data as necessary. */
  void (*init_pipe)       (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);
  /** this resets the params to factory defaults. used at the beginning of each history synch. */
  /** this commits (a mutex will be locked to synch gegl/gui) the given history params to the gegl pipe piece. */
  void (*commit_params)   (struct dt_iop_module_t *self, struct dt_iop_params_t *params, struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);
  /** this is the chance to update default parameters, after the full raw is loaded. */
  void (*reload_defaults) (struct dt_iop_module_t *self);
  /** this destroys all (gegl etc) resources needed by the piece of the pipeline. */
  void (*cleanup_pipe)    (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);
  void (*modify_roi_in)   (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const struct dt_iop_roi_t *roi_out, struct dt_iop_roi_t *roi_in);
  void (*modify_roi_out)  (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, struct dt_iop_roi_t *roi_out, const struct dt_iop_roi_t *roi_in);

  /** this is the temp homebrew callback to operations, as long as gegl is so slow.
    * x,y, and scale are just given for orientation in the framebuffer. i and o are
    * scaled to the same size width*height and contain a max of 3 floats. other color
    * formats may be filled by this callback, if the pipeline can handle it. */
  void (*process)         (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out);
  /** the opencl equivalent of process(). */
  void (*process_cl)      (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out, void *cl_mem_in, void **cl_mem_out);
}
dt_iop_module_t;

/** loads and inits the modules in the plugins/ directory. */
GList *dt_iop_load_modules(struct dt_develop_t *dev);
/** calls module->cleanup and closes the dl connection. */
void dt_iop_unload_module(dt_iop_module_t *module);
/** updates the gui params and the enabled switch. */
void dt_iop_gui_update(dt_iop_module_t *module);
/** commits params and updates piece hash. */
void dt_iop_commit_params(dt_iop_module_t *module, struct dt_iop_params_t *params, struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);
/** creates a label widget for the expander, with callback to enable/disable this module. */
GtkWidget *dt_iop_gui_get_expander(dt_iop_module_t *module);
/** requests the focus for this plugin (to draw overlays over the center image) */
void dt_iop_request_focus(dt_iop_module_t *module);
/** loads default settings from database. */
void dt_iop_load_default_params(dt_iop_module_t *module);

/** let plugins have breakpoints: */
int dt_iop_breakpoint(struct dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe);


/** for homebrew pixel pipe: zoom pixel array. */
void dt_iop_clip_and_zoom(float *out, const float *const in, const struct dt_iop_roi_t *const roi_out, const struct dt_iop_roi_t * const roi_in, const int32_t out_stride, const int32_t in_stride);

/** clip and zoom mosaiced image without demosaicing it uint16_t -> float4 */
void dt_iop_clip_and_zoom_demosaic_half_size(float *out, const uint16_t *const in, const struct dt_iop_roi_t *const roi_out, const struct dt_iop_roi_t * const roi_in, const int32_t out_stride, const int32_t in_stride, const unsigned int filters);
void dt_iop_clip_and_zoom_demosaic_half_size_f(float *out, const float *const in, const struct dt_iop_roi_t *const roi_out, const struct dt_iop_roi_t * const roi_in, const int32_t out_stride, const int32_t in_stride, const unsigned int filters);

/** as dt_iop_clip_and_zoom, but for rgba 8-bit channels. */
void dt_iop_clip_and_zoom_8(const uint8_t *i, int32_t ix, int32_t iy, int32_t iw, int32_t ih, int32_t ibw, int32_t ibh,
                                  uint8_t *o, int32_t ox, int32_t oy, int32_t ow, int32_t oh, int32_t obw, int32_t obh);

void dt_iop_YCbCr_to_RGB(const float *yuv, float *rgb);
void dt_iop_RGB_to_YCbCr(const float *rgb, float *yuv);

#endif
