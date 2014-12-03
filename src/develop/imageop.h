/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.
    copyright (c) 2011 henrik andersson.
    copyright (c) 2012 tobias ellinghaus.

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

#include <gmodule.h>
#include <gtk/gtk.h>
#include <sched.h>
#include <stdint.h>

#include "common/darktable.h"
#include "common/introspection.h"
#include "common/opencl.h"
#include "control/settings.h"
#include "develop/pixelpipe.h"
#include "dtgtk/togglebutton.h"

struct dt_develop_t;
struct dt_dev_pixelpipe_t;
struct dt_dev_pixelpipe_iop_t;
struct dt_iop_roi_t;
struct dt_develop_blend_params_t;
struct dt_develop_tiling_t;

/** module group */
typedef enum dt_iop_group_t
{
  IOP_GROUP_NONE = 0,
  IOP_GROUP_BASIC = 1 << 0,
  IOP_GROUP_COLOR = 1 << 1,
  IOP_GROUP_CORRECT = 1 << 2,
  IOP_GROUP_EFFECT = 1 << 3,
  IOP_GROUP_TONE = 1 << 4,
  IOP_SPECIAL_GROUP_ACTIVE_PIPE = 1 << 5,
  IOP_SPECIAL_GROUP_USER_DEFINED = 1 << 6
} dt_iop_group_t;
#define IOP_GROUP_ALL (IOP_GROUP_BASIC | IOP_GROUP_COLOR | IOP_GROUP_CORRECT | IOP_GROUP_EFFECT)

/** module tags */
typedef enum dt_iop_tags_t
{
  IOP_TAG_NONE = 0,
  IOP_TAG_DISTORT = 1 << 0,
  IOP_TAG_DECORATION = 1 << 1,

  // might be some other filters togglable by user?
  // IOP_TAG_SLOW       = 1<<2,
  // IOP_TAG_DETAIL_FIX = 1<<3,
} dt_iop_tags_t;

/** module tags */
typedef enum dt_iop_flags_t
{
  IOP_FLAGS_NONE = 0,

  /** Flag for the iop module to be enabled/included by default when creating a style */
  IOP_FLAGS_INCLUDE_IN_STYLES = 1 << 0,
  IOP_FLAGS_SUPPORTS_BLENDING = 1 << 1, // Does provide blending modes
  IOP_FLAGS_DEPRECATED = 1 << 2,
  IOP_FLAGS_BLEND_ONLY_LIGHTNESS
  = 1 << 3, // Does only blend with L-channel in Lab space. Keeps a, b of original image.
  IOP_FLAGS_ALLOW_TILING = 1 << 4, // Does allow tile-wise processing (valid for CPU and GPU processing)
  IOP_FLAGS_HIDDEN = 1 << 5,       // Hide the iop from userinterface
  IOP_FLAGS_TILING_FULL_ROI
  = 1 << 6, // Tiling code has to expect arbitrary roi's for this module (incl. flipping, mirroring etc.)
  IOP_FLAGS_ONE_INSTANCE = 1 << 7, // The module doesn't support multiple instances
  IOP_FLAGS_PREVIEW_NON_OPENCL
  = 1 << 8, // Preview pixelpipe of this module must not run on GPU but always on CPU
  IOP_FLAGS_NO_HISTORY_STACK = 1 << 9, // This iop will never show up in the history stack
  IOP_FLAGS_NO_MASKS = 1 << 10         // The module doesn't support masks (used with SUPPORT_BLENDING)
} dt_iop_flags_t;

/** status of a module*/
typedef enum dt_iop_module_state_t
{
  dt_iop_state_HIDDEN = 0, // keep first
  dt_iop_state_ACTIVE,
  dt_iop_state_FAVORITE,
  dt_iop_state_LAST

} dt_iop_module_state_t;

typedef void dt_iop_gui_data_t;
typedef void dt_iop_data_t;
typedef void dt_iop_global_data_t;

// params to be used to collect histogram
typedef struct dt_dev_histogram_collection_params_t
{
  /** histogram_collect: if NULL, correct is set; else should be set manually */
  const struct dt_iop_roi_t *roi;
  /** count of histogram bins. */
  uint32_t bins_count;
} dt_dev_histogram_collection_params_t;

// params used to collect histogram during last histogram capture
typedef struct dt_dev_histogram_stats_t
{
  /** count of histogram bins. */
  uint32_t bins_count;
  /** count of pixels sampled during histogram capture. */
  uint32_t pixels;
  /** count of channels: 1 for RAW, 3 for rgb/Lab. */
  uint32_t ch;
} dt_dev_histogram_stats_t;

/** when to collect histogram */
typedef enum dt_dev_request_flags_t
{
  DT_REQUEST_NONE = 0,
  DT_REQUEST_ON = 1 << 0,
  DT_REQUEST_ONLY_IN_GUI = 1 << 1
} dt_dev_request_flags_t;

/** color picker request */
typedef enum dt_dev_request_colorpick_flags_t
{
  DT_REQUEST_COLORPICK_OFF = 0,         // off
  DT_REQUEST_COLORPICK_MODULE = 1 << 0, // requested by module (should take precedence)
  DT_REQUEST_COLORPICK_BLEND = 1 << 1   // requested by parametric blending gui
} dt_dev_request_colorpick_flags_t;

/** part of the module which only contains the cached dlopen stuff. */
struct dt_iop_module_so_t;
struct dt_iop_module_t;
typedef struct dt_iop_module_so_t
{
  /** opened module. */
  GModule *module;
  /** string identifying this operation. */
  dt_dev_operation_t op;
  /** other stuff that may be needed by the module, not only in gui mode. inited only once, has to be
   * read-only then. */
  dt_iop_global_data_t *data;
  /** gui is also only inited once at startup. */
  dt_iop_gui_data_t *gui_data;
  /** which results in this widget here, too. */
  GtkWidget *widget;

  /** this initializes static, hardcoded presets for this module and is called only once per run of dt. */
  void (*init_presets)(struct dt_iop_module_so_t *self);
  /** called once per module, at startup. */
  void (*init_global)(struct dt_iop_module_so_t *self);
  /** called once per module, at shutdown. */
  void (*cleanup_global)(struct dt_iop_module_so_t *self);
  /** called once per module, at startup. */
  int (*introspection_init)(struct dt_iop_module_so_t *self, int api_version);

  /** callbacks, loaded once, referenced by the instances. */
  int (*version)();
  const char *(*name)();
  int (*groups)();
  int (*flags)();

  int (*operation_tags)();
  int (*operation_tags_filter)();

  int (*output_bpp)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                    struct dt_dev_pixelpipe_iop_t *piece);
  void (*tiling_callback)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                          const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out,
                          struct dt_develop_tiling_t *tiling);

  void (*gui_reset)(struct dt_iop_module_t *self);
  void (*gui_update)(struct dt_iop_module_t *self);
  void (*gui_init)(struct dt_iop_module_t *self);
  void (*gui_cleanup)(struct dt_iop_module_t *self);
  void (*gui_post_expose)(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                          int32_t pointerx, int32_t pointery);
  void (*gui_focus)(struct dt_iop_module_t *self, gboolean in);
  /** Optional callback for keyboard accelerators */
  void (*init_key_accels)(struct dt_iop_module_so_t *so);
  void (*original_init_key_accels)(struct dt_iop_module_so_t *so);
  void (*connect_key_accels)(struct dt_iop_module_t *self);
  void (*original_connect_key_accels)(struct dt_iop_module_t *self);
  void (*disconnect_key_accels)(struct dt_iop_module_t *self);

  int (*mouse_leave)(struct dt_iop_module_t *self);
  int (*mouse_moved)(struct dt_iop_module_t *self, double x, double y, double pressure, int which);
  int (*button_released)(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state);
  int (*button_pressed)(struct dt_iop_module_t *self, double x, double y, double pressure, int which,
                        int type, uint32_t state);
  int (*scrolled)(struct dt_iop_module_t *self, double x, double y, int up, uint32_t state);
  void (*configure)(struct dt_iop_module_t *self, int width, int height);

  void (*init)(struct dt_iop_module_t *self); // this MUST set params_size!
  void (*original_init)(struct dt_iop_module_t *self);
  void (*cleanup)(struct dt_iop_module_t *self);
  void (*init_pipe)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                    struct dt_dev_pixelpipe_iop_t *piece);
  void (*commit_params)(struct dt_iop_module_t *self, dt_iop_params_t *params,
                        struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);
  void (*reload_defaults)(struct dt_iop_module_t *self);
  void (*cleanup_pipe)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                       struct dt_dev_pixelpipe_iop_t *piece);
  void (*modify_roi_in)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                        const struct dt_iop_roi_t *roi_out, struct dt_iop_roi_t *roi_in);
  void (*modify_roi_out)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                         struct dt_iop_roi_t *roi_out, const struct dt_iop_roi_t *roi_in);
  int (*legacy_params)(struct dt_iop_module_t *self, const void *const old_params, const int old_version,
                       void *new_params, const int new_version);

  void (*process)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i, void *o,
                  const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out);
  void (*process_tiling)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i, void *o,
                         const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out,
                         const int bpp);
  int (*process_cl)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i, void *o,
                    const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out);
  int (*process_tiling_cl)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i,
                           void *o, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out,
                           const int bpp);

  int (*distort_transform)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, float *points,
                           size_t points_count);
  int (*distort_backtransform)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                               float *points, size_t points_count);

  // introspection related callbacks
  gboolean have_introspection;
  dt_introspection_t *(*get_introspection)();
  dt_introspection_field_t *(*get_introspection_linear)();
  void *(*get_p)(const void *param, const char *name);
  dt_introspection_field_t *(*get_f)(const char *name);

} dt_iop_module_so_t;

typedef struct dt_iop_module_t
{
  /** opened module. */
  GModule *module;
  /** string identifying this operation. */
  dt_dev_operation_t op;
  /** used to identify this module in the history stack. */
  int32_t instance;
  /** order in which plugins are stacked. */
  int32_t priority;
  /** module sets this if the enable checkbox should be hidden. */
  int32_t hide_enable_button;
  /** set to DT_REQUEST_COLORPICK_MODULE if you want an input color picked during next eval. gui mode only. */
  dt_dev_request_colorpick_flags_t request_color_pick;
  /** (bitwise) set if you want an histogram generated during next eval */
  dt_dev_request_flags_t request_histogram;
  /** set to source for histogram */
  dt_dev_pixelpipe_type_t request_histogram_source;
  /** set histogram generation params */
  dt_dev_histogram_collection_params_t histogram_params;
  /** stats of captured histogram */
  dt_dev_histogram_stats_t histogram_stats;
  /** set to 1 if you want the mask to be transferred into alpha channel during next eval. gui mode only. */
  int32_t request_mask_display;
  /** set to 1 if you want the blendif mask to be suppressed in the module in focus. gui mode only. */
  int32_t suppress_mask;
  /** bounding box in which the mean color is requested. */
  float color_picker_box[4];
  /** single point to pick if in point mode */
  float color_picker_point[2];
  /** place to store the picked color of module input. */
  float picked_color[3], picked_color_min[3], picked_color_max[3];
  /** place to store the picked color of module output (before blending). */
  float picked_output_color[3], picked_output_color_min[3], picked_output_color_max[3];
  /** pointer to pre-module histogram data; if available: histogram_bins_count bins with 4 channels each */
  uint32_t *histogram;
  /** maximum levels in histogram, one per channel */
  uint32_t histogram_max[4];
  /** reference for dlopened libs. */
  darktable_t *dt;
  /** the module is used in this develop module. */
  struct dt_develop_t *dev;
  /** non zero if this node should be processed. */
  int32_t enabled, default_enabled;
  /** parameters for the operation. will be replaced by history revert. */
  dt_iop_params_t *params, *default_params;
  /** size of individual params struct. */
  int32_t params_size;
  /** parameters needed if a gui is attached. will be NULL if in export/batch mode. */
  dt_iop_gui_data_t *gui_data;
  /** other stuff that may be needed by the module, not only in gui mode. */
  dt_iop_global_data_t *data;
  /** blending params */
  struct dt_develop_blend_params_t *blend_params, *default_blendop_params;
  /** holder for blending ui control */
  gpointer blend_data;
  /** child widget which is added to the GtkExpander. copied from module_so_t. */
  GtkWidget *widget;
  /** off button, somewhere in header, common to all plug-ins. */
  GtkDarktableToggleButton *off;
  /** this is the module header, contains labe and buttons */
  GtkWidget *header;

  /** button used to show/hide this module in the plugin list. */
  dt_iop_module_state_t state;
  /** expander containing the widget and flag to store expanded state */
  GtkWidget *expander;
  gboolean expanded;
  /** reset parameters button */
  GtkWidget *reset_button;
  /** show preset menu button */
  GtkWidget *presets_button;
  /** fusion slider */
  GtkWidget *fusion_slider;
  /** list of closures: show, enable/disable */
  GSList *accel_closures;
  GSList *accel_closures_local;
  gboolean local_closures_connected;
  /** the corresponding SO object */
  dt_iop_module_so_t *so;

  /** multi-instances things */
  int multi_priority; // user may change this
  char multi_name[128]; // user may change this name
  gboolean multi_show_close;
  gboolean multi_show_up;
  gboolean multi_show_down;
  GtkWidget *duplicate_button;
  GtkWidget *multimenu_button;

  /** version of the parameters in the database. */
  int (*version)();
  /** get name of the module, to be translated. */
  const char *(*name)();
  /** get the groups this module belongs to. */
  int (*groups)();
  /** get the iop module flags. */
  int (*flags)();

  int (*operation_tags)();

  int (*operation_tags_filter)();
  /** how many bytes per pixel in the output. */
  int (*output_bpp)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                    struct dt_dev_pixelpipe_iop_t *piece);
  /** report back info for tiling: memory usage and overlap. Memory usage: factor * input_size + overhead */
  void (*tiling_callback)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                          const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out,
                          struct dt_develop_tiling_t *tiling);

  /** callback methods for gui. */
  /** synch gtk interface with gui params, if necessary. */
  void (*gui_update)(struct dt_iop_module_t *self);
  /** reset ui to defaults */
  void (*gui_reset)(struct dt_iop_module_t *self);
  /** construct widget. */
  void (*gui_init)(struct dt_iop_module_t *self);
  /** destroy widget. */
  void (*gui_cleanup)(struct dt_iop_module_t *self);
  /** optional method called after darkroom expose. */
  void (*gui_post_expose)(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                          int32_t pointerx, int32_t pointery);
  /** optional callback to be notified if the module acquires gui focus/loses it. */
  void (*gui_focus)(struct dt_iop_module_t *self, gboolean in);

  /** optional event callbacks */
  int (*mouse_leave)(struct dt_iop_module_t *self);
  int (*mouse_moved)(struct dt_iop_module_t *self, double x, double y, double pressure, int which);
  int (*button_released)(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state);
  int (*button_pressed)(struct dt_iop_module_t *self, double x, double y, double pressure, int which,
                        int type, uint32_t state);
  int (*key_pressed)(struct dt_iop_module_t *self, uint16_t which);
  int (*scrolled)(struct dt_iop_module_t *self, double x, double y, int up, uint32_t state);
  void (*configure)(struct dt_iop_module_t *self, int width, int height);

  void (*init)(struct dt_iop_module_t *self); // this MUST set params_size!
  void (*original_init)(struct dt_iop_module_t *self);
  void (*cleanup)(struct dt_iop_module_t *self);
  /** this inits the piece of the pipe, allocing piece->data as necessary. */
  void (*init_pipe)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                    struct dt_dev_pixelpipe_iop_t *piece);
  /** this resets the params to factory defaults. used at the beginning of each history synch. */
  /** this commits (a mutex will be locked to synch gegl/gui) the given history params to the gegl pipe piece.
   */
  void (*commit_params)(struct dt_iop_module_t *self, dt_iop_params_t *params,
                        struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);
  /** this is the chance to update default parameters, after the full raw is loaded. */
  void (*reload_defaults)(struct dt_iop_module_t *self);
  /** this destroys all (gegl etc) resources needed by the piece of the pipeline. */
  void (*cleanup_pipe)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                       struct dt_dev_pixelpipe_iop_t *piece);
  void (*modify_roi_in)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                        const struct dt_iop_roi_t *roi_out, struct dt_iop_roi_t *roi_in);
  void (*modify_roi_out)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                         struct dt_iop_roi_t *roi_out, const struct dt_iop_roi_t *roi_in);
  int (*legacy_params)(struct dt_iop_module_t *self, const void *const old_params, const int old_version,
                       void *new_params, const int new_version);

  /** this is the temp homebrew callback to operations, as long as gegl is so slow.
    * x,y, and scale are just given for orientation in the framebuffer. i and o are
    * scaled to the same size width*height and contain a max of 3 floats. other color
    * formats may be filled by this callback, if the pipeline can handle it. */
  void (*process)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i, void *o,
                  const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out);
  /** a tiling variant of process(). */
  void (*process_tiling)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i, void *o,
                         const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out,
                         const int bpp);
  /** the opencl equivalent of process(). */
  int (*process_cl)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i, void *o,
                    const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out);
  /** a tiling variant of process_cl(). */
  int (*process_tiling_cl)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i,
                           void *o, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out,
                           const int bpp);

  /** this functions are used for distort iop
   * points is an array of float {x1,y1,x2,y2,...}
   * size is 2*points_count */
  /** points before the iop is applied => point after processed */
  int (*distort_transform)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, float *points,
                           size_t points_count);
  /** reverse points after the iop is applied => point before process */
  int (*distort_backtransform)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                               float *points, size_t points_count);

  /** Key accelerator registration callbacks */
  void (*connect_key_accels)(struct dt_iop_module_t *self);
  void (*original_connect_key_accels)(struct dt_iop_module_t *self);
  void (*disconnect_key_accels)(struct dt_iop_module_t *self);

  // introspection related data
  gboolean have_introspection;
  dt_introspection_t *(*get_introspection)();
  dt_introspection_field_t *(*get_introspection_linear)();
  void *(*get_p)(const void *param, const char *name);
  dt_introspection_field_t *(*get_f)(const char *name);

} dt_iop_module_t;

/** loads and inits the modules in the plugins/ directory. */
void dt_iop_load_modules_so();
/** cleans up the dlopen refs. */
void dt_iop_unload_modules_so();
/** returns a list of instances referencing stuff loaded in load_modules_so. */
GList *dt_iop_load_modules(struct dt_develop_t *dev);
int dt_iop_load_module(dt_iop_module_t *module, dt_iop_module_so_t *module_so, struct dt_develop_t *dev);
gint sort_plugins(gconstpointer a, gconstpointer b);
/** calls module->cleanup and closes the dl connection. */
void dt_iop_cleanup_module(dt_iop_module_t *module);
/** initialize pipe. */
void dt_iop_init_pipe(struct dt_iop_module_t *module, struct dt_dev_pixelpipe_t *pipe,
                      struct dt_dev_pixelpipe_iop_t *piece);
/** checks if iop do have an ui */
gboolean dt_iop_is_hidden(dt_iop_module_t *module);
/** checks whether iop is shown in specified group */
gboolean dt_iop_shown_in_group(dt_iop_module_t *module, uint32_t group);
/** cleans up gui of module and of blendops */
void dt_iop_gui_cleanup_module(dt_iop_module_t *module);
/** updates the gui params and the enabled switch. */
void dt_iop_gui_update(dt_iop_module_t *module);
/** reset the ui to its defaults */
void dt_iop_gui_reset(dt_iop_module_t *module);
/** set expanded state of iop */
void dt_iop_gui_set_expanded(dt_iop_module_t *module, gboolean expanded, gboolean collapse_others);
/** refresh iop according to set expanded state */
void dt_iop_gui_update_expanded(dt_iop_module_t *module);
/** change module state */
void dt_iop_gui_set_state(dt_iop_module_t *module, dt_iop_module_state_t state);

void dt_iop_gui_update_header(dt_iop_module_t *module);

/** commits params and updates piece hash. */
void dt_iop_commit_params(dt_iop_module_t *module, dt_iop_params_t *params,
                          struct dt_develop_blend_params_t *blendop_params, struct dt_dev_pixelpipe_t *pipe,
                          struct dt_dev_pixelpipe_iop_t *piece);
/** creates a label widget for the expander, with callback to enable/disable this module. */
GtkWidget *dt_iop_gui_get_expander(dt_iop_module_t *module);
/** get the widget of plugin ui in expander */
GtkWidget *dt_iop_gui_get_widget(dt_iop_module_t *module);
/** get the eventbox of plugin ui in expander */
GtkWidget *dt_iop_gui_get_pluginui(dt_iop_module_t *module);

/** requests the focus for this plugin (to draw overlays over the center image) */
void dt_iop_request_focus(dt_iop_module_t *module);
/** loads default settings from database. */
void dt_iop_load_default_params(dt_iop_module_t *module);
/** reloads certain gui/param defaults when the image was switched. */
void dt_iop_reload_defaults(dt_iop_module_t *module);

/*
 * must be called in dt_dev_change_image() to fix wrong histogram in levels
 * just after switching images and before full redraw
 */
void dt_iop_cleanup_histogram(gpointer data, gpointer user_data);

/** let plugins have breakpoints: */
int dt_iop_breakpoint(struct dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe);

/** allow plugins to relinquish CPU and go to sleep for some time */
void dt_iop_nap(int32_t usec);

/** colorspace enums */
typedef enum dt_iop_colorspace_type_t
{
  iop_cs_RAW,
  iop_cs_Lab,
  iop_cs_rgb
} dt_iop_colorspace_type_t;

/** find which colorspace the module works within */
dt_iop_colorspace_type_t dt_iop_module_colorspace(const dt_iop_module_t *module);

/** flip according to orientation bits, also zoom to given size. */
void dt_iop_flip_and_zoom_8(const uint8_t *in, int32_t iw, int32_t ih, uint8_t *out, int32_t ow, int32_t oh,
                            const dt_image_orientation_t orientation, uint32_t *width, uint32_t *height);

/** for homebrew pixel pipe: zoom pixel array. */
void dt_iop_clip_and_zoom(float *out, const float *const in, const struct dt_iop_roi_t *const roi_out,
                          const struct dt_iop_roi_t *const roi_in, const int32_t out_stride,
                          const int32_t in_stride);
#ifdef HAVE_OPENCL
int dt_iop_clip_and_zoom_cl(int devid, cl_mem dev_out, cl_mem dev_in,
                            const struct dt_iop_roi_t *const roi_out,
                            const struct dt_iop_roi_t *const roi_in);
#endif

/** clip and zoom mosaiced image without demosaicing it uint16_t -> float4 */
void dt_iop_clip_and_zoom_demosaic_half_size(float *out, const uint16_t *const in,
                                             const struct dt_iop_roi_t *const roi_out,
                                             const struct dt_iop_roi_t *const roi_in,
                                             const int32_t out_stride, const int32_t in_stride,
                                             const uint32_t filters);

void dt_iop_clip_and_zoom_demosaic_half_size_f(float *out, const float *const in,
                                               const struct dt_iop_roi_t *const roi_out,
                                               const struct dt_iop_roi_t *const roi_in,
                                               const int32_t out_stride, const int32_t in_stride,
                                               const uint32_t filters, const float clip);

/** x-trans sensor downscaling */
void dt_iop_clip_and_zoom_demosaic_third_size_xtrans(float *out, const uint16_t *const in,
                                                     const struct dt_iop_roi_t *const roi_out,
                                                     const struct dt_iop_roi_t *const roi_in,
                                                     const int32_t out_stride, const int32_t in_stride,
                                                     const uint8_t (*const xtrans)[6]);

void dt_iop_clip_and_zoom_demosaic_third_size_xtrans_f(float *out, const float *const in,
                                                       const struct dt_iop_roi_t *const roi_out,
                                                       const struct dt_iop_roi_t *const roi_in,
                                                       const int32_t out_stride, const int32_t in_stride,
                                                       const uint8_t (*const xtrans)[6]);

/** as dt_iop_clip_and_zoom, but for rgba 8-bit channels. */
void dt_iop_clip_and_zoom_8(const uint8_t *i, int32_t ix, int32_t iy, int32_t iw, int32_t ih, int32_t ibw,
                            int32_t ibh, uint8_t *o, int32_t ox, int32_t oy, int32_t ow, int32_t oh,
                            int32_t obw, int32_t obh);

void dt_iop_YCbCr_to_RGB(const float *yuv, float *rgb);
void dt_iop_RGB_to_YCbCr(const float *rgb, float *yuv);

dt_iop_module_t *get_colorout_module();

/** returns the localized plugin name for a given op name. must not be freed. */
gchar *dt_iop_get_localized_name(const gchar *op);

/** takes four points (x,y) in two arrays and fills the cubic coefficients a, such that y = [X] * a, where
  * [X] is the matrix containing all x^3 x^2 x^1 x^0 lines for all four x. */
void dt_iop_estimate_cubic(const float *const x, const float *const y, float *a);

/** evaluates the cubic fit, i.e. returns y = a^t [x^3 x^2 x^1 1] */
static inline float dt_iop_eval_cubic(const float *const a, const float x)
{
  // could be sse4.1 _mm_dot_ps
  const float x4[4] = { x * x * x, x * x, x, 1.0f };
  return a[3] * x4[3] + a[2] * x4[2] + a[1] * x4[1] + a[0] * x4[0];
}

/** estimates an exponential form f(x) = a*x^g from a few (num) points (x, y).
 *  the largest point should be (1.0, y) to really get good data. */
static inline void dt_iop_estimate_exp(const float *const x, const float *const y, const int num, float *coeff)
{
  // map every thing to y = y0*(x/x0)^g
  // and fix (x0,y0) as the last point.
  // assume (x,y) pairs are ordered by ascending x, so this is the last point:
  float x0 = x[num - 1], y0 = y[num - 1];

  float g = 0.0f;
  int cnt = 0;
  // solving for g yields
  // g = log(y/y0)/log(x/x0)
  //
  // average that over the course of the other samples:
  for(int k = 0; k < num - 1; k++)
  {
    const float yy = y[k] / y0, xx = x[k] / x0;
    if(yy > 0.0f && xx > 0.0f)
    {
      const float gg = logf(y[k] / y0) / log(x[k] / x0);
      g += gg;
      cnt++;
    }
  }
  if(cnt)
    g *= 1.0f / cnt;
  else
    g = 1.0f;
  coeff[0] = 1.0f / x0;
  coeff[1] = y0;
  coeff[2] = g;
}

/** evaluates the exp fit. */
static inline float dt_iop_eval_exp(const float *const coeff, const float x)
{
  return coeff[1] * powf(x * coeff[0], coeff[2]);
}

/** Connects common accelerators to an iop module */
void dt_iop_connect_common_accels(dt_iop_module_t *module);

/** Copy alpha channel 1:1 from input to output */
static inline void dt_iop_alpha_copy(const void *ivoid, void *ovoid, const int width, const int height)
{
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(ovoid, ivoid)
#endif
  for(int j = 0; j < height; j++)
  {
    const float *in = ((const float *)ivoid) + (size_t)4 * width * j + 3;
    float *out = ((float *)ovoid) + (size_t)4 * width * j + 3;
    for(int i = 0; i < width; i++)
    {
      *out = *in;
      out += 4;
      in += 4;
    }
  }
}

#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
