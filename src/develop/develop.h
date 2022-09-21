/*
    This file is part of darktable,
    Copyright (C) 2009-2021 darktable developers.

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

#include <cairo.h>
#include <glib.h>
#include <inttypes.h>
#include <stdint.h>

#include "common/darktable.h"
#include "common/dtpthread.h"
#include "common/image.h"
#include "control/settings.h"
#include "develop/imageop.h"

struct dt_iop_module_t;

typedef struct dt_dev_history_item_t
{
  struct dt_iop_module_t *module; // pointer to image operation module
  int32_t enabled;                // switched respective module on/off
  dt_iop_params_t *params;        // parameters for this operation
  struct dt_develop_blend_params_t *blend_params;
  char op_name[20];
  int iop_order;
  int multi_priority;
  char multi_name[128];
  GList *forms; // snapshot of dt_develop_t->forms
  int num; // num of history on database
  int32_t focus_hash;             // used to determine whether or not to start a new item or to merge down
} dt_dev_history_item_t;

typedef enum dt_dev_overexposed_colorscheme_t
{
  DT_DEV_OVEREXPOSED_BLACKWHITE = 0,
  DT_DEV_OVEREXPOSED_REDBLUE = 1,
  DT_DEV_OVEREXPOSED_PURPLEGREEN = 2
} dt_dev_overexposed_colorscheme_t;

typedef enum dt_dev_overlay_colors_t
{
  DT_DEV_OVERLAY_GRAY = 0,
  DT_DEV_OVERLAY_RED = 1,
  DT_DEV_OVERLAY_GREEN = 2,
  DT_DEV_OVERLAY_YELLOW = 3,
  DT_DEV_OVERLAY_CYAN = 4,
  DT_DEV_OVERLAY_MAGENTA = 5
} dt_dev_overlay_colors_t;

typedef enum dt_dev_rawoverexposed_mode_t {
  DT_DEV_RAWOVEREXPOSED_MODE_MARK_CFA = 0,
  DT_DEV_RAWOVEREXPOSED_MODE_MARK_SOLID = 1,
  DT_DEV_RAWOVEREXPOSED_MODE_FALSECOLOR = 2,
} dt_dev_rawoverexposed_mode_t;

typedef enum dt_dev_rawoverexposed_colorscheme_t {
  DT_DEV_RAWOVEREXPOSED_RED = 0,
  DT_DEV_RAWOVEREXPOSED_GREEN = 1,
  DT_DEV_RAWOVEREXPOSED_BLUE = 2,
  DT_DEV_RAWOVEREXPOSED_BLACK = 3
} dt_dev_rawoverexposed_colorscheme_t;

typedef enum dt_dev_transform_direction_t
{
  DT_DEV_TRANSFORM_DIR_ALL = 0,
  DT_DEV_TRANSFORM_DIR_FORW_INCL = 1,
  DT_DEV_TRANSFORM_DIR_FORW_EXCL = 2,
  DT_DEV_TRANSFORM_DIR_BACK_INCL = 3,
  DT_DEV_TRANSFORM_DIR_BACK_EXCL = 4
} dt_dev_transform_direction_t;

typedef enum dt_dev_pixelpipe_status_t
{
  DT_DEV_PIXELPIPE_DIRTY = 0,   // history stack changed or image new
  DT_DEV_PIXELPIPE_RUNNING = 1, // pixelpipe is running
  DT_DEV_PIXELPIPE_VALID = 2,   // pixelpipe has finished; valid result
  DT_DEV_PIXELPIPE_INVALID = 3  // pixelpipe has finished; invalid result
} dt_dev_pixelpipe_status_t;

typedef enum dt_dev_pixelpipe_display_mask_t
{
  DT_DEV_PIXELPIPE_DISPLAY_NONE = 0,
  DT_DEV_PIXELPIPE_DISPLAY_MASK = 1 << 0,
  DT_DEV_PIXELPIPE_DISPLAY_CHANNEL = 1 << 1,
  DT_DEV_PIXELPIPE_DISPLAY_OUTPUT = 1 << 2,
  DT_DEV_PIXELPIPE_DISPLAY_L = 1 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_a = 2 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_b = 3 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_R = 4 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_G = 5 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_B = 6 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_GRAY = 7 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_LCH_C = 8 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_LCH_h = 9 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_HSL_H = 10 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_HSL_S = 11 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_HSL_l = 12 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Jz = 13 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Cz = 14 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_hz = 15 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU = 16 << 3, // show module's output without processing by later iops
  DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU_MONO = 17 << 3, // same as above but specific for pre-demosaic to stay monochrome
  DT_DEV_PIXELPIPE_DISPLAY_ANY = 0xff << 2,
  DT_DEV_PIXELPIPE_DISPLAY_STICKY = 1 << 16
} dt_dev_pixelpipe_display_mask_t;

typedef enum dt_develop_detail_mmask_t
{
  DT_DEV_DETAIL_MASK_NONE = 0,
  DT_DEV_DETAIL_MASK_REQUIRED = 1,
  DT_DEV_DETAIL_MASK_DEMOSAIC = 2,
  DT_DEV_DETAIL_MASK_RAWPREPARE = 4
} dt_develop_detail_mask_t;

typedef enum dt_clipping_preview_mode_t
{
  DT_CLIPPING_PREVIEW_GAMUT = 0,
  DT_CLIPPING_PREVIEW_ANYRGB = 1,
  DT_CLIPPING_PREVIEW_LUMINANCE = 2,
  DT_CLIPPING_PREVIEW_SATURATION = 3
} dt_clipping_preview_mode_t;

typedef struct dt_dev_proxy_exposure_t
{
  struct dt_iop_module_t *module;
  void (*set_exposure)(struct dt_iop_module_t *exp, const float exposure);
  float (*get_exposure)(struct dt_iop_module_t *exp);
  void (*set_black)(struct dt_iop_module_t *exp, const float black);
  float (*get_black)(struct dt_iop_module_t *exp);
} dt_dev_proxy_exposure_t;

struct dt_dev_pixelpipe_t;
typedef struct dt_develop_t
{
  int32_t gui_attached; // != 0 if the gui should be notified of changes in hist stack and modules should be
                        // gui_init'ed.
  int32_t gui_leaving;  // set if everything is scheduled to shut down.
  int32_t gui_synch;    // set by the render threads if gui_update should be called in the modules.
  int32_t focus_hash;   // determines whether to start a new history item or to merge down.
  gboolean preview_loading, preview2_loading, image_loading, history_updating, image_force_reload, first_load;
  gboolean preview_input_changed, preview2_input_changed;

  dt_dev_pixelpipe_status_t image_status, preview_status, preview2_status;
  int32_t image_invalid_cnt;
  uint32_t timestamp;
  uint32_t average_delay;
  uint32_t preview_average_delay;
  uint32_t preview2_average_delay;
  struct dt_iop_module_t *gui_module; // this module claims gui expose/event callbacks.
  float preview_downsampling;         // < 1.0: optionally downsample preview

  // width, height: dimensions of window
  int32_t width, height;

  // image processing pipeline with caching
  struct dt_dev_pixelpipe_t *pipe, *preview_pipe, *preview2_pipe;
  dt_pthread_mutex_t pipe_mutex, preview_pipe_mutex,
      preview2_pipe_mutex; // these are locked while the pipes are still in use

  // image under consideration, which
  // is copied each time an image is changed. this means we have some information
  // always cached (might be out of sync, so stars are not reliable), but for the iops
  // it's quite a convenience to access trivial stuff which is constant anyways without
  // calling into the cache explicitly. this should never be accessed directly, but
  // by the iop through the copy their respective pixelpipe holds, for thread-safety.
  dt_image_t image_storage;

  // history stack
  dt_pthread_mutex_t history_mutex;
  int32_t history_end;
  GList *history;

  // operations pipeline
  int32_t iop_instance;
  GList *iop;
  // iop's to be deleted
  GList *alliop;

  // iop order
  int iop_order_version;
  GList *iop_order_list;

  // profiles info
  GList *allprofile_info;

  // histogram for display.
  uint32_t *histogram_pre_tonecurve, *histogram_pre_levels;
  uint32_t histogram_pre_tonecurve_max, histogram_pre_levels_max;

  // list of forms iop can use for masks or whatever
  GList *forms;
  struct dt_masks_form_t *form_visible;
  struct dt_masks_form_gui_t *form_gui;
  // all forms to be linked here for cleanup:
  GList *allforms;

  //full preview stuff
  int full_preview;
  int full_preview_last_zoom, full_preview_last_closeup;
  float full_preview_last_zoom_x, full_preview_last_zoom_y;
  struct dt_iop_module_t *full_preview_last_module;
  int full_preview_masks_state;

  // darkroom border size
  int32_t border_size;
  int32_t orig_width, orig_height;

  /* proxy for communication between plugins and develop/darkroom */
  struct
  {
    // list of exposure iop instances, with plugin hooks, used by histogram dragging functions
    // each element is dt_dev_proxy_exposure_t
    dt_dev_proxy_exposure_t exposure;

    // modulegroups plugin hooks
    struct
    {
      struct dt_lib_module_t *module;
      /* switch module group */
      void (*set)(struct dt_lib_module_t *self, uint32_t group);
      /* get current module group */
      uint32_t (*get)(struct dt_lib_module_t *self);
      /* get activated module group */
      uint32_t (*get_activated)(struct dt_lib_module_t *self);
      /* test if iop group flags matches modulegroup */
      gboolean (*test)(struct dt_lib_module_t *self, uint32_t group, struct dt_iop_module_t *module);
      /* switch to modulegroup */
      void (*switch_group)(struct dt_lib_module_t *self, struct dt_iop_module_t *module);
      /* update modulegroup visibility */
      void (*update_visibility)(struct dt_lib_module_t *self);
      /* test if module is preset in one of the current groups */
      gboolean (*test_visible)(struct dt_lib_module_t *self, gchar *module);
      /* add or remove module or widget in current quick access list */
      gboolean (*basics_module_toggle)(struct dt_lib_module_t *self, GtkWidget *widget, gboolean doit);
    } modulegroups;

    // snapshots plugin hooks
    struct
    {
      // this flag is set by snapshot plugin to signal that expose of darkroom
      // should store cairo surface as snapshot to disk using filename.
      gboolean request;
      const gchar *filename;
    } snapshot;

    // masks plugin hooks
    struct
    {
      struct dt_lib_module_t *module;
      /* treview list refresh */
      void (*list_change)(struct dt_lib_module_t *self);
      void (*list_remove)(struct dt_lib_module_t *self, int formid, int parentid);
      void (*list_update)(struct dt_lib_module_t *self);
      /* selected forms change */
      void (*selection_change)(struct dt_lib_module_t *self, struct dt_iop_module_t *module, const int selectid, const int throw_event);
    } masks;

    // what is the ID of the module currently doing pipeline chromatic adaptation ?
    // this is to prevent multiple modules/instances from doing white balance globally.
    // only used to display warnings in GUI of modules that should probably not be doing white balance
    struct dt_iop_module_t *chroma_adaptation;

    // is the WB module using D65 illuminant and not doing full chromatic adaptation ?
    gboolean wb_is_D65;
    dt_aligned_pixel_t wb_coeffs;

  } proxy;

  // for the overexposure indicator
  struct
  {
    GtkWidget *floating_window, *button; // yes, having gtk stuff in here is ugly. live with it.

    gboolean enabled;
    dt_dev_overexposed_colorscheme_t colorscheme;
    float lower;
    float upper;
    dt_clipping_preview_mode_t mode;
  } overexposed;

  // for the raw overexposure indicator
  struct
  {
    GtkWidget *floating_window, *button; // yes, having gtk stuff in here is ugly. live with it.

    gboolean enabled;
    dt_dev_rawoverexposed_mode_t mode;
    dt_dev_rawoverexposed_colorscheme_t colorscheme;
    float threshold;
  } rawoverexposed;

  // ISO 12646-compliant colour assessment conditions
  struct
  {
    GtkWidget *button; // yes, ugliness is the norm. what did you expect ?
    gboolean enabled;
  } iso_12646;

  // the display profile related things (softproof, gamut check, profiles ...)
  struct
  {
    GtkWidget *floating_window, *softproof_button, *gamut_button;
  } profile;

  // second darkroom window related things
  struct
  {
    GtkWidget *second_wnd;
    GtkWidget *widget;
    int width, height;
    double dpi, dpi_factor, ppd, ppd_thb;

    GtkWidget *button;

    dt_dev_zoom_t zoom;
    int closeup;
    float zoom_x, zoom_y;
    float zoom_scale;

    double button_x;
    double button_y;
  } second_window;

  int mask_form_selected_id; // select a mask inside an iop
  gboolean darkroom_skip_mouse_events; // skip mouse events for masks
} dt_develop_t;

void dt_dev_init(dt_develop_t *dev, int32_t gui_attached);
void dt_dev_cleanup(dt_develop_t *dev);

float dt_dev_get_preview_downsampling();
void dt_dev_process_image_job(dt_develop_t *dev);
void dt_dev_process_preview_job(dt_develop_t *dev);
void dt_dev_process_preview2_job(dt_develop_t *dev);
// launch jobs above
void dt_dev_process_image(dt_develop_t *dev);
void dt_dev_process_preview(dt_develop_t *dev);
void dt_dev_process_preview2(dt_develop_t *dev);

void dt_dev_load_image(dt_develop_t *dev, const uint32_t imgid);
void dt_dev_reload_image(dt_develop_t *dev, const uint32_t imgid);
/** checks if provided imgid is the image currently in develop */
int dt_dev_is_current_image(dt_develop_t *dev, uint32_t imgid);
const dt_dev_history_item_t *dt_dev_get_history_item(dt_develop_t *dev, const char *op);
void dt_dev_add_history_item_ext(dt_develop_t *dev, struct dt_iop_module_t *module, gboolean enable, gboolean no_image);
void dt_dev_add_history_item(dt_develop_t *dev, struct dt_iop_module_t *module, gboolean enable);
void dt_dev_add_new_history_item(dt_develop_t *dev, struct dt_iop_module_t *module, gboolean enable);
void dt_dev_add_masks_history_item_ext(dt_develop_t *dev, struct dt_iop_module_t *_module, gboolean _enable, gboolean no_image);
void dt_dev_add_masks_history_item(dt_develop_t *dev, struct dt_iop_module_t *_module, gboolean enable);
void dt_dev_reload_history_items(dt_develop_t *dev);
void dt_dev_pop_history_items_ext(dt_develop_t *dev, int32_t cnt);
void dt_dev_pop_history_items(dt_develop_t *dev, int32_t cnt);
void dt_dev_write_history_ext(dt_develop_t *dev, const int imgid);
void dt_dev_write_history(dt_develop_t *dev);
void dt_dev_read_history_ext(dt_develop_t *dev, const int imgid, gboolean no_image);
void dt_dev_read_history(dt_develop_t *dev);
void dt_dev_free_history_item(gpointer data);
void dt_dev_invalidate_history_module(GList *list, struct dt_iop_module_t *module);

void dt_dev_invalidate(dt_develop_t *dev);
// also invalidates preview (which is unaffected by resize/zoom/pan)
void dt_dev_invalidate_all(dt_develop_t *dev);
void dt_dev_set_histogram(dt_develop_t *dev);
void dt_dev_set_histogram_pre(dt_develop_t *dev);
void dt_dev_get_history_item_label(dt_dev_history_item_t *hist, char *label, const int cnt);
void dt_dev_reprocess_all(dt_develop_t *dev);
void dt_dev_reprocess_center(dt_develop_t *dev);
void dt_dev_reprocess_preview(dt_develop_t *dev);

void dt_dev_get_processed_size(const dt_develop_t *dev, int *procw, int *proch);
void dt_dev_check_zoom_bounds(dt_develop_t *dev, float *zoom_x, float *zoom_y, dt_dev_zoom_t zoom,
                              int closeup, float *boxw, float *boxh);
float dt_dev_get_zoom_scale(dt_develop_t *dev, dt_dev_zoom_t zoom, int closeup_factor, int mode);
void dt_dev_get_pointer_zoom_pos(dt_develop_t *dev, const float px, const float py, float *zoom_x,
                                 float *zoom_y);

void dt_dev_configure(dt_develop_t *dev, int wd, int ht);
void dt_dev_invalidate_from_gui(dt_develop_t *dev);

/*
 * exposure plugin hook, set the exposure and the black level
 */

/** check if exposure iop hooks are available */
gboolean dt_dev_exposure_hooks_available(dt_develop_t *dev);
/** reset exposure to defaults */
void dt_dev_exposure_reset_defaults(dt_develop_t *dev);
/** set exposure */
void dt_dev_exposure_set_exposure(dt_develop_t *dev, const float exposure);
/** get exposure */
float dt_dev_exposure_get_exposure(dt_develop_t *dev);
/** set exposure black level */
void dt_dev_exposure_set_black(dt_develop_t *dev, const float black);
/** get exposure black level */
float dt_dev_exposure_get_black(dt_develop_t *dev);

/*
 * modulegroups plugin hooks
 */
/** switch to modulegroup of module */
void dt_dev_modulegroups_switch(dt_develop_t *dev, struct dt_iop_module_t *module);
/** update modulegroup visibility */
void dt_dev_modulegroups_update_visibility(dt_develop_t *dev);
/** set the active modulegroup */
void dt_dev_modulegroups_set(dt_develop_t *dev, uint32_t group);
/** get the active modulegroup */
uint32_t dt_dev_modulegroups_get(dt_develop_t *dev);
/** get the activated modulegroup */
uint32_t dt_dev_modulegroups_get_activated(dt_develop_t *dev);
/** test if iop group flags matches modulegroup */
gboolean dt_dev_modulegroups_test(dt_develop_t *dev, uint32_t group, struct dt_iop_module_t *module);
/** reorder the module list */
void dt_dev_reorder_gui_module_list(dt_develop_t *dev);
/** test if the iop is visible in current groups layout **/
gboolean dt_dev_modulegroups_is_visible(dt_develop_t *dev, gchar *module);
/** add or remove module or widget in current quick access list **/
int dt_dev_modulegroups_basics_module_toggle(dt_develop_t *dev, GtkWidget *widget, gboolean doit);

/** request snapshot */
void dt_dev_snapshot_request(dt_develop_t *dev, const char *filename);

/** update gliding average for pixelpipe delay */
void dt_dev_average_delay_update(const dt_times_t *start, uint32_t *average_delay);

/*
 * masks plugin hooks
 */
void dt_dev_masks_list_change(dt_develop_t *dev);
void dt_dev_masks_list_update(dt_develop_t *dev);
void dt_dev_masks_list_remove(dt_develop_t *dev, int formid, int parentid);
void dt_dev_masks_selection_change(dt_develop_t *dev, struct dt_iop_module_t *module, const int selectid, const int throw_event);

/*
 * multi instances
 */
/** duplicate a existent module */
struct dt_iop_module_t *dt_dev_module_duplicate(dt_develop_t *dev, struct dt_iop_module_t *base);
/** remove an existent module */
void dt_dev_module_remove(dt_develop_t *dev, struct dt_iop_module_t *module);
/** update "show" values of the multi instance part (show_move, show_delete, ...) */
void dt_dev_module_update_multishow(dt_develop_t *dev, struct dt_iop_module_t *module);
/** same, but for all modules */
void dt_dev_modules_update_multishow(dt_develop_t *dev);
/** generates item multi-instance name */
gchar *dt_history_item_get_name(const struct dt_iop_module_t *module);
gchar *dt_history_item_get_name_html(const struct dt_iop_module_t *module);

/*
 * distort functions
 */
/** apply all transforms to the specified points (in preview pipe space) */
int dt_dev_distort_transform(dt_develop_t *dev, float *points, size_t points_count);
/** reverse apply all transforms to the specified points (in preview pipe space) */
int dt_dev_distort_backtransform(dt_develop_t *dev, float *points, size_t points_count);
/** same fct, but we can specify iop with priority between pmin and pmax */
int dt_dev_distort_transform_plus(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction,
                                  float *points, size_t points_count);
/** same fct, but can only be called from a distort_transform function called by dt_dev_distort_transform_plus */
int dt_dev_distort_transform_locked(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order,
                                    const int transf_direction, float *points, size_t points_count);
/** same fct as dt_dev_distort_backtransform, but we can specify iop with priority between pmin and pmax */
int dt_dev_distort_backtransform_plus(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction,
                                      float *points, size_t points_count);
/** same fct, but can only be called from a distort_backtransform function called by dt_dev_distort_backtransform_plus */
int dt_dev_distort_backtransform_locked(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order,
                                    const int transf_direction, float *points, size_t points_count);

/** get the iop_pixelpipe instance corresponding to the iop in the given pipe */
struct dt_dev_pixelpipe_iop_t *dt_dev_distort_get_iop_pipe(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe,
                                                           struct dt_iop_module_t *module);
/*
 * hash functions
 */
/** generate hash value out of all module settings of pixelpipe */
uint64_t dt_dev_hash(dt_develop_t *dev);
/** same function, but we can specify iop with priority between pmin and pmax */
uint64_t dt_dev_hash_plus(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction);
/** wait until hash value found in hash matches hash value defined by dev/pipe/pmin/pmax with timeout */
int dt_dev_wait_hash(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction, dt_pthread_mutex_t *lock,
                     const volatile uint64_t *const hash);
/** synchronize pixelpipe by means hash values by waiting with timeout and potential reprocessing */
int dt_dev_sync_pixelpipe_hash(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction, dt_pthread_mutex_t *lock,
                               const volatile uint64_t *const hash);
/** generate hash value out of module settings of all distorting modules of pixelpipe */
uint64_t dt_dev_hash_distort(dt_develop_t *dev);
/** same function, but we can specify iop with priority between pmin and pmax */
uint64_t dt_dev_hash_distort_plus(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction);
/** same as dt_dev_wait_hash but only for distorting modules */
int dt_dev_wait_hash_distort(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction, dt_pthread_mutex_t *lock,
                             const volatile uint64_t *const hash);
/** same as dt_dev_sync_pixelpipe_hash but only for distorting modules */
int dt_dev_sync_pixelpipe_hash_distort (dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction, dt_pthread_mutex_t *lock,
                                        const volatile uint64_t *const hash);

/*
 * second darkroom window zoom heplers
*/
dt_dev_zoom_t dt_second_window_get_dev_zoom(dt_develop_t *dev);
void dt_second_window_set_dev_zoom(dt_develop_t *dev, const dt_dev_zoom_t value);
int dt_second_window_get_dev_closeup(dt_develop_t *dev);
void dt_second_window_set_dev_closeup(dt_develop_t *dev, const int value);
float dt_second_window_get_dev_zoom_x(dt_develop_t *dev);
void dt_second_window_set_dev_zoom_x(dt_develop_t *dev, const float value);
float dt_second_window_get_dev_zoom_y(dt_develop_t *dev);
void dt_second_window_set_dev_zoom_y(dt_develop_t *dev, const float value);
float dt_second_window_get_free_zoom_scale(dt_develop_t *dev);
float dt_second_window_get_zoom_scale(dt_develop_t *dev, const dt_dev_zoom_t zoom, const int closeup_factor,
                                      const int preview);
void dt_second_window_set_zoom_scale(dt_develop_t *dev, const float value);
void dt_second_window_get_processed_size(const dt_develop_t *dev, int *procw, int *proch);
void dt_second_window_check_zoom_bounds(dt_develop_t *dev, float *zoom_x, float *zoom_y, const dt_dev_zoom_t zoom,
                                        const int closeup, float *boxww, float *boxhh);

/*
 *   history undo support helpers for darkroom
 */

/* all history change must be enclosed into a start / end call */
void dt_dev_undo_start_record(dt_develop_t *dev);
void dt_dev_undo_end_record(dt_develop_t *dev);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

