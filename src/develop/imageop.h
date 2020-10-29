/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

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

#pragma once

#include <gmodule.h>
#include <gtk/gtk.h>
#include <sched.h>
#include <stdint.h>

/** region of interest */
typedef struct dt_iop_roi_t
{
  int x, y, width, height;
  float scale;
} dt_iop_roi_t;

#include "common/darktable.h"
#include "common/introspection.h"
#include "common/opencl.h"
#include "control/settings.h"
#include "develop/pixelpipe.h"
#include "dtgtk/togglebutton.h"

struct dt_develop_t;
struct dt_dev_pixelpipe_t;
struct dt_dev_pixelpipe_iop_t;
struct dt_develop_blend_params_t;
struct dt_develop_tiling_t;
struct dt_iop_color_picker_t;

typedef enum dt_iop_module_header_icons_t
{
  IOP_MODULE_SWITCH = 0,
  IOP_MODULE_ICON,
  IOP_MODULE_LABEL,
  IOP_MODULE_INSTANCE,
  IOP_MODULE_RESET,
  IOP_MODULE_PRESETS,
  IOP_MODULE_LAST
} dt_iop_module_header_icons_t;

/** module group */
typedef enum dt_iop_group_t
{
  IOP_GROUP_NONE = 0,
  // pre 3.4 layout
  IOP_GROUP_BASIC = 1 << 0,
  IOP_GROUP_TONE = 1 << 1,
  IOP_GROUP_COLOR = 1 << 2,
  IOP_GROUP_CORRECT = 1 << 3,
  IOP_GROUP_EFFECT = 1 << 4,
  // post 3.4 default layout
  IOP_GROUP_TECHNICAL = 1 << 5,
  IOP_GROUP_GRADING = 1 << 6,
  IOP_GROUP_EFFECTS = 1 << 7,
  // special group
  IOP_SPECIAL_GROUP_ACTIVE_PIPE = 1 << 8
} dt_iop_group_t;

/** module tags */
typedef enum dt_iop_tags_t
{
  IOP_TAG_NONE = 0,
  IOP_TAG_DISTORT = 1 << 0,
  IOP_TAG_DECORATION = 1 << 1,
  IOP_TAG_CLIPPING = 1 << 2,

  // might be some other filters togglable by user?
  // IOP_TAG_SLOW       = 1<<3,
  // IOP_TAG_DETAIL_FIX = 1<<3,
} dt_iop_tags_t;

/** module tags */
typedef enum dt_iop_flags_t
{
  IOP_FLAGS_NONE = 0,

  /** Flag for the iop module to be enabled/included by default when creating a style */
  IOP_FLAGS_INCLUDE_IN_STYLES  = 1 << 0,
  IOP_FLAGS_SUPPORTS_BLENDING  = 1 << 1,  // Does provide blending modes
  IOP_FLAGS_DEPRECATED         = 1 << 2,
  IOP_FLAGS_ALLOW_TILING       = 1 << 4,  // Does allow tile-wise processing (valid for CPU and GPU processing)
  IOP_FLAGS_HIDDEN             = 1 << 5,  // Hide the iop from userinterface
  IOP_FLAGS_TILING_FULL_ROI    = 1 << 6,  // Tiling code has to expect arbitrary roi's for this module (incl. flipping, mirroring etc.)
  IOP_FLAGS_ONE_INSTANCE       = 1 << 7,  // The module doesn't support multiple instances
  IOP_FLAGS_PREVIEW_NON_OPENCL = 1 << 8,  // Preview pixelpipe of this module must not run on GPU but always on CPU
  IOP_FLAGS_NO_HISTORY_STACK   = 1 << 9,  // This iop will never show up in the history stack
  IOP_FLAGS_NO_MASKS           = 1 << 10, // The module doesn't support masks (used with SUPPORT_BLENDING)
  IOP_FLAGS_FENCE              = 1 << 11, // No module can be moved pass this one
  IOP_FLAGS_ALLOW_FAST_PIPE    = 1 << 12, // Module can work with a fast pipe
  IOP_FLAGS_UNSAFE_COPY        = 1 << 13  // Unsafe to copy as part of history
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

/** color picker request */
typedef enum dt_dev_request_colorpick_flags_t
{
  DT_REQUEST_COLORPICK_OFF = 0,   // off
  DT_REQUEST_COLORPICK_MODULE = 1 // requested by module (should take precedence)
} dt_dev_request_colorpick_flags_t;

/** colorspace enums, must be in synch with dt_iop_colorspace_type_t in color_conversion.cl */
typedef enum dt_iop_colorspace_type_t
{
  iop_cs_NONE = -1,
  iop_cs_RAW = 0,
  iop_cs_Lab = 1,
  iop_cs_rgb = 2,
  iop_cs_LCh = 3,
  iop_cs_HSL = 4
} dt_iop_colorspace_type_t;

/** part of the module which only contains the cached dlopen stuff. */
struct dt_iop_module_so_t;
struct dt_iop_module_t;
typedef struct dt_iop_module_so_t
{
  // !!! MUST BE KEPT IN SYNC WITH src/iop/iop_api.h !!!

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
  /** button used to show/hide this module in the plugin list. */
  dt_iop_module_state_t state;

  /** this initializes static, hardcoded presets for this module and is called only once per run of dt. */
  void (*init_presets)(struct dt_iop_module_so_t *self);
  /** called once per module, at startup. */
  void (*init_global)(struct dt_iop_module_so_t *self);
  /** called once per module, at shutdown. */
  void (*cleanup_global)(struct dt_iop_module_so_t *self);
  /** called once per module, at startup. */
  int (*introspection_init)(struct dt_iop_module_so_t *self, int api_version);

  /** callbacks, loaded once, referenced by the instances. */
  int (*version)(void);
  const char *(*name)(void);
  int (*default_group)(void);
  int (*flags)(void);

  const char *(*description)(void);
  /* should return a string with 5 lines:
     line 1 : summary of what it does
     line 2 : oriented creative or corrective ?
     line 3 : working space
     line 4 : input space
     line 5 : output space
  */

  int (*operation_tags)(void);
  int (*operation_tags_filter)(void);

  /** what do the iop want as an input? */
  void (*input_format)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                       struct dt_dev_pixelpipe_iop_t *piece, struct dt_iop_buffer_dsc_t *dsc);
  /** what will it output? */
  void (*output_format)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                        struct dt_dev_pixelpipe_iop_t *piece, struct dt_iop_buffer_dsc_t *dsc);

  /** what default colorspace this iop use? */
  int (*default_colorspace)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                       struct dt_dev_pixelpipe_iop_t *piece);
  /** what input colorspace it expects? */
  int (*input_colorspace)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                       struct dt_dev_pixelpipe_iop_t *piece);
  /** what will it output? */
  int (*output_colorspace)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                        struct dt_dev_pixelpipe_iop_t *piece);
  /** what colorspace the blend module operates with? */
  int (*blend_colorspace)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                        struct dt_dev_pixelpipe_iop_t *piece);

  void (*tiling_callback)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                          const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out,
                          struct dt_develop_tiling_t *tiling);

  void (*gui_reset)(struct dt_iop_module_t *self);
  void (*gui_update)(struct dt_iop_module_t *self);
  void (*gui_init)(struct dt_iop_module_t *self);
  void (*color_picker_apply)(struct dt_iop_module_t *self, GtkWidget *picker, struct dt_dev_pixelpipe_iop_t *piece);
  void (*gui_changed)(struct dt_iop_module_t *self, GtkWidget *widget, void *previous);
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
  GSList *(*mouse_actions)(struct dt_iop_module_t *self);

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
  void (*change_image)(struct dt_iop_module_t *self);
  void (*reload_defaults)(struct dt_iop_module_t *self);
  void (*cleanup_pipe)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                       struct dt_dev_pixelpipe_iop_t *piece);
  void (*modify_roi_in)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                        const struct dt_iop_roi_t *roi_out, struct dt_iop_roi_t *roi_in);
  void (*modify_roi_out)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                         struct dt_iop_roi_t *roi_out, const struct dt_iop_roi_t *roi_in);
  int (*legacy_params)(struct dt_iop_module_t *self, const void *const old_params, const int old_version,
                       void *new_params, const int new_version);
  // allow to select a shape inside an iop
  void (*masks_selection_changed)(struct dt_iop_module_t *self, const int form_selected_id);

  void (*process)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const void *const i,
                  void *const o, const struct dt_iop_roi_t *const roi_in,
                  const struct dt_iop_roi_t *const roi_out);
  void (*process_tiling)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                         const void *const i, void *const o, const struct dt_iop_roi_t *const roi_in,
                         const struct dt_iop_roi_t *const roi_out, const int bpp);
  void (*process_plain)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                        const void *const i, void *const o, const struct dt_iop_roi_t *const roi_in,
                        const struct dt_iop_roi_t *const roi_out);
  void (*process_sse2)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                       const void *const i, void *const o, const struct dt_iop_roi_t *const roi_in,
                       const struct dt_iop_roi_t *const roi_out);
  int (*process_cl)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const void *const i,
                    void *const o, const struct dt_iop_roi_t *const roi_in,
                    const struct dt_iop_roi_t *const roi_out);
  int (*process_tiling_cl)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                           const void *const i, void *const o, const struct dt_iop_roi_t *const roi_in,
                           const struct dt_iop_roi_t *const roi_out, const int bpp);

  int (*distort_transform)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, float *points,
                           size_t points_count);
  int (*distort_backtransform)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                               float *points, size_t points_count);
  void (*distort_mask)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const float *const in,
                       float *const out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out);

  // introspection related callbacks
  gboolean have_introspection;
  dt_introspection_t *(*get_introspection)(void);
  dt_introspection_field_t *(*get_introspection_linear)(void);
  void *(*get_p)(const void *param, const char *name);
  dt_introspection_field_t *(*get_f)(const char *name);

} dt_iop_module_so_t;

typedef struct dt_iop_module_t
{
  // !!! MUST BE KEPT IN SYNC WITH src/iop/iop_api.h !!!

  /** opened module. */
  GModule *module;
  /** string identifying this operation. */
  dt_dev_operation_t op;
  /** used to identify this module in the history stack. */
  int32_t instance;
  /** order of the module on the pipe. the pipe will be sorted by iop_order. */
  int iop_order;
  /** module sets this if the enable checkbox should be hidden. */
  int32_t hide_enable_button;
  /** set to DT_REQUEST_COLORPICK_MODULE if you want an input color picked during next eval. gui mode only. */
  dt_dev_request_colorpick_flags_t request_color_pick;
  /** (bitwise) set if you want an histogram generated during next eval */
  dt_dev_request_flags_t request_histogram;
  /** set to 1 if you want the mask to be transferred into alpha channel during next eval. gui mode only. */
  int request_mask_display;
  /** set to 1 if you want the blendif mask to be suppressed in the module in focus. gui mode only. */
  int32_t suppress_mask;
  /** color picker proxy */
  struct dt_iop_color_picker_t *picker;
  /** bounding box in which the mean color is requested. */
  float color_picker_box[4];
  /** single point to pick if in point mode */
  float color_picker_point[2];
  /** place to store the picked color of module input. */
  float picked_color[4], picked_color_min[4], picked_color_max[4];
  /** place to store the picked color of module output (before blending). */
  float picked_output_color[4], picked_output_color_min[4], picked_output_color_max[4];
  /** pointer to pre-module histogram data; if available: histogram_bins_count bins with 4 channels each */
  uint32_t *histogram;
  /** stats of captured histogram */
  dt_dev_histogram_stats_t histogram_stats;
  /** maximum levels in histogram, one per channel */
  uint32_t histogram_max[4];
  /** requested colorspace for the histogram, valid options are:
   * iop_cs_NONE: module colorspace
   * iop_cs_LCh: for Lab modules
   */
  dt_iop_colorspace_type_t histogram_cst;
  /** scale the histogram so the middle grey is at .5 */
  int histogram_middle_grey;
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
  dt_iop_global_data_t *global_data;
  /** blending params */
  struct dt_develop_blend_params_t *blend_params, *default_blendop_params;
  /** holder for blending ui control */
  gpointer blend_data;
  struct {
    struct {
      /** if this module generates a mask, is it used later on? needed to decide if the mask should be stored.
          maps dt_iop_module_t* -> id
      */
      GHashTable *users;
      /** the masks this module has to offer. maps id -> name */
      GHashTable *masks;
    } source;
    struct {
      /** the module that provides the raster mask (if any). keep in sync with blend_params! */
      struct dt_iop_module_t *source;
      int id;
    } sink;
  } raster_mask;
  /** child widget which is added to the GtkExpander. copied from module_so_t. */
  GtkWidget *widget;
  /** off button, somewhere in header, common to all plug-ins. */
  GtkDarktableToggleButton *off;
  /** this is the module header, contains label and buttons */
  GtkWidget *header;

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
  gboolean multi_show_new;
  GtkWidget *duplicate_button;
  GtkWidget *multimenu_button;

  /** delayed-event handling */
  guint timeout_handle;

  /** version of the parameters in the database. */
  int (*version)(void);
  /** get name of the module, to be translated. */
  const char *(*name)(void);
  /** get the default group this module belongs to. */
  int (*default_group)(void);
  /** get the iop module flags. */
  int (*flags)(void);

  /** get a descriptive text used for example in a tooltip in more modules */
  const char *(*description)(void);

  int (*operation_tags)(void);

  int (*operation_tags_filter)(void);
  void (*input_format)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                       struct dt_dev_pixelpipe_iop_t *piece, struct dt_iop_buffer_dsc_t *dsc);
  /** what will it output? */
  void (*output_format)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                        struct dt_dev_pixelpipe_iop_t *piece, struct dt_iop_buffer_dsc_t *dsc);

  /** what default colorspace this iop use? */
  int (*default_colorspace)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                       struct dt_dev_pixelpipe_iop_t *piece);
  /** what input colorspace it expects? */
  int (*input_colorspace)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                       struct dt_dev_pixelpipe_iop_t *piece);
  /** what will it output? */
  int (*output_colorspace)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                        struct dt_dev_pixelpipe_iop_t *piece);
  /** what colorspace the blend module operates with? */
  int (*blend_colorspace)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
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
  /** apply color picker results */
  void (*color_picker_apply)(struct dt_iop_module_t *self, GtkWidget *picker, struct dt_dev_pixelpipe_iop_t *piece);
  /** called by standard widget callbacks after value changed */
  void (*gui_changed)(struct dt_iop_module_t *self, GtkWidget *widget, void *previous);
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
  /** this commits (a mutex will be locked to synch pipe/gui) the given history params to the pixelpipe piece.
   */
  void (*commit_params)(struct dt_iop_module_t *self, dt_iop_params_t *params,
                        struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);
  /** this is the chance to update default parameters, after the full raw is loaded. */
  void (*reload_defaults)(struct dt_iop_module_t *self);
  /** called after the image has changed in darkroom */
  void (*change_image)(struct dt_iop_module_t *self);
  /** this destroys all resources needed by the piece of the pixelpipe. */
  void (*cleanup_pipe)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
                       struct dt_dev_pixelpipe_iop_t *piece);
  void (*modify_roi_in)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                        const struct dt_iop_roi_t *roi_out, struct dt_iop_roi_t *roi_in);
  void (*modify_roi_out)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                         struct dt_iop_roi_t *roi_out, const struct dt_iop_roi_t *roi_in);
  int (*legacy_params)(struct dt_iop_module_t *self, const void *const old_params, const int old_version,
                       void *new_params, const int new_version);
  // allow to select a shape inside an iop
  void (*masks_selection_changed)(struct dt_iop_module_t *self, const int form_selected_id);

  /** this is the temp homebrew callback to operations.
    * x,y, and scale are just given for orientation in the framebuffer. i and o are
    * scaled to the same size width*height and contain a max of 3 floats. other color
    * formats may be filled by this callback, if the pipeline can handle it. */
  /** the simplest variant of process(). you can only use OpenMP SIMD here, no intrinsics */
  void (*process)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const void *const i,
                  void *const o, const struct dt_iop_roi_t *const roi_in,
                  const struct dt_iop_roi_t *const roi_out);
  /** a tiling variant of process(). */
  void (*process_tiling)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                         const void *const i, void *const o, const struct dt_iop_roi_t *const roi_in,
                         const struct dt_iop_roi_t *const roi_out, const int bpp);
  /** WARNING: in IOP implementation, it is called process()!!! */
  /** the simplest variant of process(). you can only use OpenMP SIMD here, no intrinsics */
  void (*process_plain)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                        const void *const i, void *const o, const struct dt_iop_roi_t *const roi_in,
                        const struct dt_iop_roi_t *const roi_out);
  /** a variant process(), that can contain SSE2 intrinsics. */
  void (*process_sse2)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                       const void *const i, void *const o, const struct dt_iop_roi_t *const roi_in,
                       const struct dt_iop_roi_t *const roi_out);
  /** the opencl equivalent of process(). */
  int (*process_cl)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const void *const i,
                    void *const o, const struct dt_iop_roi_t *const roi_in,
                    const struct dt_iop_roi_t *const roi_out);
  /** a tiling variant of process_cl(). */
  int (*process_tiling_cl)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                           const void *const i, void *const o, const struct dt_iop_roi_t *const roi_in,
                           const struct dt_iop_roi_t *const roi_out, const int bpp);

  /** this functions are used for distort iop
   * points is an array of float {x1,y1,x2,y2,...}
   * size is 2*points_count */
  /** points before the iop is applied => point after processed */
  int (*distort_transform)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, float *points,
                           size_t points_count);
  /** reverse points after the iop is applied => point before process */
  int (*distort_backtransform)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                               float *points, size_t points_count);
  /** apply the image distortion to a single channel float buffer. only needed by iops that distort the image */
  void (*distort_mask)(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const float *const in,
                       float *const out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out);

  /** Key accelerator registration callbacks */
  void (*connect_key_accels)(struct dt_iop_module_t *self);
  void (*original_connect_key_accels)(struct dt_iop_module_t *self);
  void (*disconnect_key_accels)(struct dt_iop_module_t *self);
  GSList *(*mouse_actions)(struct dt_iop_module_t *self);

  // introspection related data
  gboolean have_introspection;
  dt_introspection_t *(*get_introspection)(void);
  dt_introspection_field_t *(*get_introspection_linear)(void);
  void *(*get_p)(const void *param, const char *name);
  dt_introspection_field_t *(*get_f)(const char *name);

} dt_iop_module_t;

/** loads and inits the modules in the plugins/ directory. */
void dt_iop_load_modules_so(void);
/** cleans up the dlopen refs. */
void dt_iop_unload_modules_so(void);
/** load a module for a given .so */
int dt_iop_load_module_by_so(dt_iop_module_t *module, dt_iop_module_so_t *so, struct dt_develop_t *dev);
/** returns a list of instances referencing stuff loaded in load_modules_so. */
GList *dt_iop_load_modules_ext(struct dt_develop_t *dev, gboolean no_image);
GList *dt_iop_load_modules(struct dt_develop_t *dev);
int dt_iop_load_module(dt_iop_module_t *module, dt_iop_module_so_t *module_so, struct dt_develop_t *dev);
/** calls module->cleanup and closes the dl connection. */
void dt_iop_cleanup_module(dt_iop_module_t *module);
/** initialize pipe. */
void dt_iop_init_pipe(struct dt_iop_module_t *module, struct dt_dev_pixelpipe_t *pipe,
                      struct dt_dev_pixelpipe_iop_t *piece);
/** checks if iop do have an ui */
gboolean dt_iop_so_is_hidden(dt_iop_module_so_t *module);
gboolean dt_iop_is_hidden(dt_iop_module_t *module);
/** checks whether iop is shown in specified group */
gboolean dt_iop_shown_in_group(dt_iop_module_t *module, uint32_t group);
/** cleans up gui of module and of blendops */
void dt_iop_gui_cleanup_module(dt_iop_module_t *module);
/** updates the enable button state. (take into account module->enabled and module->hide_enable_button  */
void dt_iop_gui_set_enable_button(dt_iop_module_t *module);
/** updates the gui params and the enabled switch. */
void dt_iop_gui_update(dt_iop_module_t *module);
/** reset the ui to its defaults */
void dt_iop_gui_reset(dt_iop_module_t *module);
/** set expanded state of iop */
void dt_iop_gui_set_expanded(dt_iop_module_t *module, gboolean expanded, gboolean collapse_others);
/** refresh iop according to set expanded state */
void dt_iop_gui_update_expanded(dt_iop_module_t *module);
/** change module state */
void dt_iop_so_gui_set_state(dt_iop_module_so_t *module, dt_iop_module_state_t state);
void dt_iop_gui_set_state(dt_iop_module_t *module, dt_iop_module_state_t state);
/* duplicate module and return new instance */
dt_iop_module_t *dt_iop_gui_duplicate(dt_iop_module_t *base, gboolean copy_params);

void dt_iop_gui_update_header(dt_iop_module_t *module);

/** commits params and updates piece hash. */
void dt_iop_commit_params(dt_iop_module_t *module, dt_iop_params_t *params,
                          struct dt_develop_blend_params_t *blendop_params, struct dt_dev_pixelpipe_t *pipe,
                          struct dt_dev_pixelpipe_iop_t *piece);
void dt_iop_commit_blend_params(dt_iop_module_t *module, const struct dt_develop_blend_params_t *blendop_params);
/** make sure the raster mask is advertised if available */
void dt_iop_set_mask_mode(dt_iop_module_t *module, int mask_mode);
/** creates a label widget for the expander, with callback to enable/disable this module. */
GtkWidget *dt_iop_gui_get_expander(dt_iop_module_t *module);
/** get the widget of plugin ui in expander */
GtkWidget *dt_iop_gui_get_widget(dt_iop_module_t *module);
/** get the eventbox of plugin ui in expander */
GtkWidget *dt_iop_gui_get_pluginui(dt_iop_module_t *module);

/** requests the focus for this plugin (to draw overlays over the center image) */
void dt_iop_request_focus(dt_iop_module_t *module);
/** allocate and load default settings from introspection. */
void dt_iop_default_init(dt_iop_module_t *module);
/** loads default settings from database. */
void dt_iop_load_default_params(dt_iop_module_t *module);
/** creates the module's gui widget */
void dt_iop_gui_init(dt_iop_module_t *module);
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

/** get module by name and colorout, works only with a dev mode */
dt_iop_module_t *dt_iop_get_colorout_module(void);
/* returns the iop-module found in list with the given name */
dt_iop_module_t *dt_iop_get_module_from_list(GList *iop_list, const char *op);
dt_iop_module_t *dt_iop_get_module(const char *op);
/** returns module with op + multi_priority or NULL if not found on the list,
    if multi_priority == -1 do not checl for it */
dt_iop_module_t *dt_iop_get_module_by_op_priority(GList *modules, const char *operation, const int multi_priority);
/** returns module with op + multi_name or NULL if not found on the list,
    if multi_name == NULL do not checl for it */
dt_iop_module_t *dt_iop_get_module_by_instance_name(GList *modules, const char *operation, const char *multi_name);


/** get module flags, works in dev and lt mode */
int get_module_flags(const char *op);

/** returns the localized plugin name for a given op name. must not be freed. */
gchar *dt_iop_get_localized_name(const gchar *op);

/** Connects common accelerators to an iop module */
void dt_iop_connect_common_accels(dt_iop_module_t *module);

/** set multi_priority and update raster mask links */
void dt_iop_update_multi_priority(dt_iop_module_t *module, int new_priority);

/** iterates over the users hash table and checks if a specific mask is being used */
gboolean dt_iop_is_raster_mask_used(dt_iop_module_t *module, int id);

/** returns the previous visible module on the module list */
dt_iop_module_t *dt_iop_gui_get_previous_visible_module(dt_iop_module_t *module);
/** returns the next visible module on the module list */
dt_iop_module_t *dt_iop_gui_get_next_visible_module(dt_iop_module_t *module);

// initializes memory.darktable_iop_names
void dt_iop_set_darktable_iop_table();

/** adds keyboard accels to the first module in the pipe to handle where there are multiple instances */
void dt_iop_connect_accels_multi(dt_iop_module_so_t *module);

/** adds keyboard accels for all modules in the pipe */
void dt_iop_connect_accels_all();

/** get the module that accelerators are attached to for the current so */
dt_iop_module_t *dt_iop_get_module_accel_curr(dt_iop_module_so_t *module);

/** count instances of a module **/
int dt_iop_count_instances(dt_iop_module_so_t *module);

/** queue a refresh of the center (FULL), preview, or second-preview windows, rerunning the pixelpipe from */
/** the given module */
void dt_iop_refresh_center(dt_iop_module_t *module);
void dt_iop_refresh_preview(dt_iop_module_t *module);
void dt_iop_refresh_preview2(dt_iop_module_t *module);
void dt_iop_refresh_all(dt_iop_module_t *module);

/** queue a delayed call to dt_dev_add_history_item to capture module parameters */
void dt_iop_queue_history_update(dt_iop_module_t *module, gboolean extend_prior);
/** cancel any previously-queued history update */
void dt_iop_cancel_history_update(dt_iop_module_t *module);

/** (un)hide iop module header right side buttons */
gboolean dt_iop_show_hide_header_buttons(GtkWidget *header, GdkEventCrossing *event, gboolean show_buttons, gboolean always_hide);

#define IOP_GUI_ALLOC(module) (dt_iop_##module##_gui_data_t *)(self->gui_data = calloc(1, sizeof(dt_iop_##module##_gui_data_t)))
#define IOP_GUI_FREE free(self->gui_data); self->gui_data = NULL

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
