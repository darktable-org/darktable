/*
    This file is part of darktable,
    Copyright (C) 2009-2023 darktable developers.

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

#include "common/darktable.h"
#include "common/introspection.h"
#include "common/opencl.h"
#include "common/action.h"
#include "control/settings.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/** region of interest, needed by pixelpipe.h */
typedef struct dt_iop_roi_t
{
  int x, y, width, height;
  float scale;
} dt_iop_roi_t;

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

#include "develop/pixelpipe.h"
#include "dtgtk/togglebutton.h"

#if defined(__SSE__)
#include <xmmintrin.h> // needed for _mm_stream_ps
#else
#ifdef __cplusplus
#include <atomic>
#else
#include <stdatomic.h>
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

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
  IOP_MODULE_INSTANCE_NAME,
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
  IOP_FLAGS_INCLUDE_IN_STYLES = 1 << 0,
  IOP_FLAGS_SUPPORTS_BLENDING = 1 << 1, // Does provide blending modes
  IOP_FLAGS_DEPRECATED = 1 << 2,
  IOP_FLAGS_ALLOW_TILING = 1 << 4, // Does allow tile-wise processing (valid for CPU and GPU processing)
  IOP_FLAGS_HIDDEN = 1 << 5,       // Hide the iop from userinterface
  IOP_FLAGS_TILING_FULL_ROI
  = 1 << 6, // Tiling code has to expect arbitrary roi's for this module (incl. flipping, mirroring etc.)
  IOP_FLAGS_ONE_INSTANCE = 1 << 7,       // The module doesn't support multiple instances
  IOP_FLAGS_PREVIEW_NON_OPENCL = 1 << 8, // Preview pixelpipe of this module must not run on GPU but always on CPU
  IOP_FLAGS_NO_HISTORY_STACK = 1 << 9,   // This iop will never show up in the history stack
  IOP_FLAGS_NO_MASKS = 1 << 10,          // The module doesn't support masks (used with SUPPORT_BLENDING)
  IOP_FLAGS_FENCE = 1 << 11,             // No module can be moved pass this one
  IOP_FLAGS_ALLOW_FAST_PIPE = 1 << 12,   // Module can work with a fast pipe
  IOP_FLAGS_UNSAFE_COPY = 1 << 13,       // Unsafe to copy as part of history
  IOP_FLAGS_GUIDES_SPECIAL_DRAW = 1 << 14, // handle the grid drawing directly
  IOP_FLAGS_GUIDES_WIDGET = 1 << 15,       // require the guides widget
  IOP_FLAGS_CACHE_IMPORTANT_NOW = 1 << 16, // hints for higher priority in iop cache
  IOP_FLAGS_CACHE_IMPORTANT_NEXT = 1 << 17
} dt_iop_flags_t;

/** status of a module*/
typedef enum dt_iop_module_state_t
{
  IOP_STATE_HIDDEN = 0, // keep first
  IOP_STATE_ACTIVE,
  IOP_STATE_FAVORITE,
  IOP_STATE_LAST
} dt_iop_module_state_t;

typedef struct dt_iop_gui_data_t
{
  // "base type" for all dt_iop_XXXX_gui_data_t types used by iops to
  // avoid compiler error about different sizes of empty structs
  // between C and C++, we need at least one member
  int dummy;
} dt_iop_gui_data_t;

typedef void dt_iop_data_t;
typedef void dt_iop_global_data_t;

/** color picker request */
typedef enum dt_dev_request_colorpick_flags_t
{
  DT_REQUEST_COLORPICK_OFF = 0,   // off
  DT_REQUEST_COLORPICK_MODULE = 1 // requested by module (should take precedence)
} dt_dev_request_colorpick_flags_t;

/** colorspace enums, must be in synch with dt_iop_colorspace_type_t
 * in color_conversion.cl */
typedef enum dt_iop_colorspace_type_t
{
  IOP_CS_NONE = -1,
  IOP_CS_RAW = 0,
  IOP_CS_LAB = 1,
  IOP_CS_RGB = 2,
  IOP_CS_LCH = 3,
  IOP_CS_HSL = 4,
  IOP_CS_JZCZHZ = 5,
} dt_iop_colorspace_type_t;

/** part of the module which only contains the cached dlopen stuff. */
typedef struct dt_iop_module_so_t
{
  dt_action_t actions; // !!! NEEDS to be FIRST (to be able to cast convert)

#define INCLUDE_API_FROM_MODULE_H
#include "iop/iop_api.h"

  /** opened module. */
  GModule *module;
  /** string identifying this operation. */
  dt_dev_operation_t op;
  /** other stuff that may be needed by the module, not only in gui
   * mode. inited only once, has to be read-only then. */
  dt_iop_global_data_t *data;
  /** button used to show/hide this module in the plugin list. */
  dt_iop_module_state_t state;

  void (*process_plain)(struct dt_iop_module_t *self,
                        struct dt_dev_pixelpipe_iop_t *piece,
                        const void *const i, void *const o,
                        const struct dt_iop_roi_t *const roi_in,
                        const struct dt_iop_roi_t *const roi_out);

  // introspection related data
  gboolean have_introspection;
  // contains preset which are depending on preference (workflow)
  gboolean pref_based_presets;
} dt_iop_module_so_t;

typedef struct dt_iop_module_t
{
  dt_action_type_t actions; // !!! NEEDS to be FIRST (to be able to cast convert)

#define INCLUDE_API_FROM_MODULE_H
#include "iop/iop_api.h"

  /** opened module. */
  GModule *module;
  /** string identifying this operation. */
  dt_dev_operation_t op;
  /** used to identify this module in the history stack. */
  int32_t instance;
  /** order of the module on the pipe. the pipe will be sorted by iop_order. */
  int iop_order;
  /** module sets this if the enable checkbox should be hidden. */
  gboolean hide_enable_button;
  /** set to DT_REQUEST_COLORPICK_MODULE if you want an input color
   * picked during next eval. gui mode only. */
  dt_dev_request_colorpick_flags_t request_color_pick;
  /** (bitwise) set if you want an histogram generated during next eval */
  dt_dev_request_flags_t request_histogram;
  /** set to 1 if you want the mask to be transferred into alpha
   * channel during next eval. gui mode only. */
  dt_dev_pixelpipe_display_mask_t request_mask_display;
  /** set to 1 if you want the blendif mask to be suppressed in the
   * module in focus. gui mode only. */
  gboolean suppress_mask;
  /** place to store the picked color of module input. */
  dt_aligned_pixel_t picked_color, picked_color_min, picked_color_max;
  /** place to store the picked color of module output (before blending). */
  dt_aligned_pixel_t picked_output_color, picked_output_color_min, picked_output_color_max;
  /** pointer to pre-module histogram data; if available:
   * histogram_bins_count bins with 4 channels each */
  uint32_t *histogram;
  /** stats of captured histogram */
  dt_dev_histogram_stats_t histogram_stats;
  /** maximum levels in histogram, one per channel */
  uint32_t histogram_max[4];
  /** requested colorspace for the histogram, valid options are:
   * IOP_CS_NONE: module colorspace
   * IOP_CS_LCH: for Lab modules
   */
  dt_iop_colorspace_type_t histogram_cst;
  /** scale the histogram so the middle grey is at .5 */
  gboolean histogram_middle_grey;
  /** the module is used in this develop module. */
  struct dt_develop_t *dev;
  /** TRUE if this node should be processed. */
  gboolean enabled, default_enabled;
  /** parameters for the operation. will be replaced by history revert. */
  dt_iop_params_t *params, *default_params;
  /** size of individual params struct. */
  int32_t params_size;
  /** parameters needed if a gui is attached. will be NULL if in export/batch mode. */
  dt_iop_gui_data_t *gui_data;
  dt_pthread_mutex_t gui_lock;
  /** other stuff that may be needed by the module, not only in gui mode. */
  dt_iop_global_data_t *global_data;
  /** blending params */
  struct dt_develop_blend_params_t *blend_params, *default_blendop_params;
  /** holder for blending ui control */
  gpointer blend_data;
  struct {
    struct {
      /** if this module generates a mask, is it used later on? needed
          to decide if the mask should be stored.  maps
          dt_iop_module_t* -> id
      */
      GHashTable *users;
      /** the masks this module has to offer. maps id -> name */
      GHashTable *masks;
    } source;
    struct {
      /** the module that provides the raster mask (if any). keep in
       * sync with blend_params! */
      struct dt_iop_module_t *source;
      dt_mask_id_t id;
    } sink;
  } raster_mask;
  /** child widget which is added to the GtkExpander. copied from module_so_t. */
  GtkWidget *widget;
  /** off button, somewhere in header, common to all plug-ins. */
  GtkDarktableToggleButton *off;
  /** this is the module header, contains label and buttons */
  GtkWidget *header;
  GtkWidget *label;
  GtkWidget *instance_name;
  /** this is the module mask indicator, inside header */
  GtkWidget *mask_indicator;
  /** expander containing the widget and flag to store expanded state */
  GtkWidget *expander;
  gboolean expanded;
  /** reset parameters button */
  GtkWidget *reset_button;
  /** show preset menu button */
  GtkWidget *presets_button;
  /** fusion slider */
  GtkWidget *fusion_slider;

  /* list of instance widgets and associated actions. Bauhaus with
   * field pointer at end, starting from widget_list_bh */
  GSList *widget_list;
  GSList *widget_list_bh;

  /** show/hide guide button and combobox */
  GtkWidget *guides_toggle;
  GtkWidget *guides_combo;

  /** flag in case the module has troubles (bad settings) - if TRUE,
   * show a warning sign next to module label */
  gboolean has_trouble;
  /** the corresponding SO object */
  dt_iop_module_so_t *so;

  /** multi-instances things */
  int multi_priority; // user may change this
  char multi_name[128]; // user may change this name
  gboolean multi_name_hand_edited;
  GtkWidget *multimenu_button;

  /** delayed-event handling */
  guint label_recompute_handle;

  void (*process_plain)(struct dt_iop_module_t *self,
                        struct dt_dev_pixelpipe_iop_t *piece,
                        const void *const i,
                        void *const o,
                        const struct dt_iop_roi_t *const roi_in,
                        const struct dt_iop_roi_t *const roi_out);
  // hint for higher io cache priority
  gboolean cache_next_important;
  // introspection related data
  gboolean have_introspection;
} dt_iop_module_t;

typedef struct dt_action_target_t
{
  dt_action_t *action;
  void *target;
} dt_action_target_t;

/** loads and inits the modules in the plugins/ directory. */
void dt_iop_load_modules_so(void);
/** cleans up the dlopen refs. */
void dt_iop_unload_modules_so(void);
/** load a module for a given .so */
int dt_iop_load_module_by_so(dt_iop_module_t *module,
                             dt_iop_module_so_t *so,
                             struct dt_develop_t *dev);
/** returns a list of instances referencing stuff loaded in load_modules_so. */
GList *dt_iop_load_modules_ext(struct dt_develop_t *dev, gboolean no_image);
GList *dt_iop_load_modules(struct dt_develop_t *dev);
int dt_iop_load_module(dt_iop_module_t *module,
                       dt_iop_module_so_t *module_so,
                       struct dt_develop_t *dev);
/** calls module->cleanup and closes the dl connection. */
void dt_iop_cleanup_module(dt_iop_module_t *module);
/** initialize pipe. */
void dt_iop_init_pipe(struct dt_iop_module_t *module,
                      struct dt_dev_pixelpipe_t *pipe,
                      struct dt_dev_pixelpipe_iop_t *piece);
/** checks if iop do have an ui */
gboolean dt_iop_so_is_hidden(dt_iop_module_so_t *module);
gboolean dt_iop_is_hidden(dt_iop_module_t *module);
/** checks whether iop is shown in specified group */
gboolean dt_iop_shown_in_group(dt_iop_module_t *module, uint32_t group);
/** enter a GUI critical section by acquiring gui_data->lock **/
static inline void dt_iop_gui_enter_critical_section(dt_iop_module_t *const module)
  ACQUIRE(&module->gui_lock)
{
  dt_pthread_mutex_lock(&module->gui_lock);
}
/** leave a GUI critical section by releasing gui_data->lock **/
static inline void dt_iop_gui_leave_critical_section(dt_iop_module_t *const module)
  RELEASE(&module->gui_lock)
{
  dt_pthread_mutex_unlock(&module->gui_lock);
}
/** cleans up gui of module and of blendops */
void dt_iop_gui_cleanup_module(dt_iop_module_t *module);
/** updates the enable button state. (take into account
 * module->enabled and module->hide_enable_button */
void dt_iop_gui_set_enable_button(dt_iop_module_t *module);
/** updates the gui params and the enabled switch. */
void dt_iop_gui_update(dt_iop_module_t *module);
/** reset the ui to its defaults */
void dt_iop_gui_reset(dt_iop_module_t *module);
/** set expanded state of iop */
void dt_iop_gui_set_expanded(dt_iop_module_t *module,
                             const gboolean expanded,
                             const gboolean collapse_others);
/** refresh iop according to set expanded state */
void dt_iop_gui_update_expanded(dt_iop_module_t *module);
/** change module state */
void dt_iop_so_gui_set_state(dt_iop_module_so_t *module, dt_iop_module_state_t state);
void dt_iop_gui_set_state(dt_iop_module_t *module, dt_iop_module_state_t state);
/* duplicate module and return new instance */
dt_iop_module_t *dt_iop_gui_duplicate(dt_iop_module_t *base, gboolean copy_params);

void dt_iop_gui_update_header(dt_iop_module_t *module);

/** commits params and updates piece hash. */
void dt_iop_commit_params(dt_iop_module_t *module,
                          dt_iop_params_t *params,
                          struct dt_develop_blend_params_t *blendop_params,
                          struct dt_dev_pixelpipe_t *pipe,
                          struct dt_dev_pixelpipe_iop_t *piece);

dt_iop_module_t *dt_iop_commit_blend_params(dt_iop_module_t *module,
                                const struct dt_develop_blend_params_t *blendop_params);
/** make sure the raster mask is advertised if available */
void dt_iop_set_mask_mode(dt_iop_module_t *module, int mask_mode);
/** creates a label widget for the expander, with callback to enable/disable this module. */
void dt_iop_gui_set_expander(dt_iop_module_t *module);
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

extern const struct dt_action_def_t dt_action_def_iop;

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
    if multi_priority == -1 do not check for it */
dt_iop_module_t *dt_iop_get_module_by_op_priority(GList *modules,
                                                  const char *operation,
                                                  const int multi_priority);
/** returns module with op + multi_name or NULL if not found on the list,
    if multi_name == NULL do not check for it */
dt_iop_module_t *dt_iop_get_module_by_instance_name(GList *modules,
                                                    const char *operation,
                                                    const char *multi_name);
/** check for module name */
static inline gboolean dt_iop_module_is(const dt_iop_module_so_t *module,
                                        const char*operation)
{
  return !g_strcmp0(module->op, operation);
}

/** count instances of a module **/
int dt_iop_count_instances(dt_iop_module_so_t *module);
/** return preferred module instance for shortcuts **/
dt_iop_module_t *dt_iop_get_module_preferred_instance(dt_iop_module_so_t *module);

/** returns true if module is the first instance of this operation in the pipe */
gboolean dt_iop_is_first_instance(GList *modules, dt_iop_module_t *module);

/** return the instance name for the module, this is either the multi-name
    for instance 0 or if hand-edited. Otherwise the name is the empty string.
 */
const char *dt_iop_get_instance_name(const dt_iop_module_t *module);

/** get module flags, works in dev and lt mode */
int dt_iop_get_module_flags(const char *op);

/** returns the localized plugin name for a given op name. must not be freed. */
const gchar *dt_iop_get_localized_name(const gchar *op);
const gchar *dt_iop_get_localized_aliases(const gchar *op);

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

/** adds keyboard accels to the first module in the pipe to handle
 * where there are multiple instances */
void dt_iop_connect_accels_multi(dt_iop_module_so_t *module);

/** adds keyboard accels for all modules in the pipe */
void dt_iop_connect_accels_all();

/** get the module that accelerators are attached to for the current so */
dt_iop_module_t *dt_iop_get_module_accel_curr(dt_iop_module_so_t *module);

/** queue a refresh of the center (FULL), preview, or second-preview
 * windows, rerunning the pixelpipe from */
/** the given module */
void dt_iop_refresh_center(dt_iop_module_t *module);
void dt_iop_refresh_preview(dt_iop_module_t *module);
void dt_iop_refresh_preview2(dt_iop_module_t *module);
void dt_iop_refresh_all(dt_iop_module_t *module);

/** (un)hide iop module header right side buttons */
gboolean dt_iop_show_hide_header_buttons(dt_iop_module_t *module,
                                         GdkEventCrossing *event,
                                         gboolean show_buttons,
                                         const gboolean always_hide);

/** add/remove mask indicator to iop module header */
void add_remove_mask_indicator(dt_iop_module_t *module, gboolean add);

/** Set the trouble message for the module.  If non-empty, also flag
 ** the module as being in trouble; if empty or NULL, clear the
 ** trouble flag.  If 'toast_message' is non-NULL/non-empty, pop up a
 ** toast with that message when the module does not have a
 ** warning-label widget (use %s for the module's name).  **/
void dt_iop_set_module_trouble_message(dt_iop_module_t *module,
                                       const char *const trouble_msg,
                                       const char *const trouble_tooltip,
                                       const char *stderr_message);

// format modules description going in tooltips
const char **dt_iop_set_description(dt_iop_module_t *module,
                                    const char *main_text,
                                    const char *purpose,
                                    const char *input,
                                    const char *process,
                                    const char *output);

/** get a nice printable name. */
const char *dt_iop_colorspace_to_name(const dt_iop_colorspace_type_t type);

static inline dt_iop_gui_data_t *_iop_gui_alloc(dt_iop_module_t *module, const size_t size)
{
  // Align so that DT_ALIGNED_ARRAY may be used within gui_data struct
  module->gui_data = (dt_iop_gui_data_t*)dt_calloc_align(64, size);
  dt_pthread_mutex_init(&module->gui_lock, NULL);
  return module->gui_data;
}
#define IOP_GUI_ALLOC(module) \
  (dt_iop_##module##_gui_data_t *)_iop_gui_alloc(self,sizeof(dt_iop_##module##_gui_data_t))

#define IOP_GUI_FREE \
  dt_pthread_mutex_destroy(&self->gui_lock);if(self->gui_data){dt_free_align(self->gui_data);} self->gui_data = NULL

/** check whether we have the required number of channels in the input
 ** data; if not, copy the input buffer to the output buffer, set the
 ** module's trouble message, and return FALSE */
gboolean dt_iop_have_required_input_format(const int required_ch,
                                           struct dt_iop_module_t *const module,
                                           const int actual_pipe_ch,
                                           const void *const __restrict__ ivoid,
                                           void *const __restrict__ ovoid,
                                           const dt_iop_roi_t *const roi_in,
                                           const dt_iop_roi_t *const roi_out);

/* bring up module rename dialog */
void dt_iop_gui_rename_module(dt_iop_module_t *module);

void dt_iop_gui_changed(dt_action_t *action, GtkWidget *widget, gpointer data);

// copy the RGB channels of a pixel using nontemporal stores if
// possible; includes the 'alpha' channel as well if faster due to
// vectorization, but subsequent code should ignore the value of the
// alpha unless explicitly set afterwards (since it might not have
// been copied).  NOTE: nontemporal stores will actually be *slower*
// if we immediately access the pixel again.  This function should
// only be used when processing an entire image before doing anything
// else with the destination buffer.
static inline void copy_pixel_nontemporal(
	float *const __restrict__ out,
        const float *const __restrict__ in)
{
#if defined(__SSE__)
  _mm_stream_ps(out, *((__m128*)in));
#elif (__clang__+0 > 7) && (__clang__+0 < 10)
  for_each_channel(k,aligned(in,out:16)) __builtin_nontemporal_store(in[k],out[k]);
#else
  for_each_channel(k,aligned(in,out:16) dt_omp_nontemporal(out)) out[k] = in[k];
#endif
}

// after writing data using copy_pixel_nontemporal, it is necessary to
// ensure that the writes have completed before attempting reads from
// a different core.  This function produces the required memmory
// fence to ensure proper visibility
static inline void dt_sfence()
{
#if defined(__SSE__)
  _mm_sfence();
#else
  // the following generates an MFENCE instruction on x86/x64.  We
  // only really need SFENCE, which is less expensive, but none of the
  // other memory orders generate *any* fence instructions on x64.
#ifdef __cplusplus
  std::atomic_thread_fence(std::memory_order_seq_cst);
#else
  atomic_thread_fence(memory_order_seq_cst);
#endif
#endif
}

// if the copy_pixel_nontemporal() writes were inside an OpenMP
// parallel loop, the OpenMP parallelization will have performed a
// memory fence before resuming single-threaded operation, so a
// dt_sfence would be superfluous.  But if compiled without OpenMP
// parallelization, we should play it safe and emit a memory fence.
// This function should be used right after a parallelized for loop,
// where it will produce a barrier only if needed.
#ifdef _OPENMP
#define dt_omploop_sfence()
#else
#define dt_omploop_sfence() dt_sfence()
#endif

#ifdef __SSE2__
static inline unsigned int dt_mm_enable_flush_zero()
{
  // flush denormals to zero for masking to avoid performance penalty
  // if there are a lot of zero values in the mask
  const unsigned int oldMode = _MM_GET_FLUSH_ZERO_MODE();
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
  return oldMode;
}

static inline void dt_mm_restore_flush_zero(const unsigned int mode)
{
  _MM_SET_FLUSH_ZERO_MODE(mode);
}

#define DT_PREFETCH(addr) _mm_prefetch(addr, _MM_HINT_T2)
#define PREFETCH_NTA(addr) _mm_prefetch(addr, _MM_HINT_NTA)

#else // no SSE2

#define dt_mm_enable_flush_zero() 0
#define dt_mm_restore_flush_zero(mode) (void)mode;

#if defined(__GNUC__)
#define DT_PREFETCH(addr) __builtin_prefetch(addr,1,1)
#define PREFETCH_NTA(addr) __builtin_prefetch(addr,1,0)
#else
#define DT_PREFETCH(addr)
#define PREFETCH_NTA(addr)
#endif

// avoid cluttering the scalar codepath with #ifdefs by hiding the dependency on SSE2
# define _mm_prefetch(where,hint)

#endif /* __SSE2__ */

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
