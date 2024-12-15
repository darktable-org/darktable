/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.

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

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct dt_iop_module_t;

typedef struct dt_dev_history_item_t
{
  struct dt_iop_module_t *module; // pointer to image operation module
  gboolean enabled;               // switched respective module on/off
  dt_iop_params_t *params;        // parameters for this operation
  struct dt_develop_blend_params_t *blend_params;
  char op_name[20];
  int iop_order;
  int multi_priority;
  char multi_name[128];
  gboolean multi_name_hand_edited;
  GList *forms;        // snapshot of dt_develop_t->forms
  int num;             // num of history on database
  gboolean focus_hash;  // used to determine whether or not to start a
                       // new item or to merge down
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
  float (*get_exposure)(struct dt_iop_module_t *exp);
  float (*get_black)(struct dt_iop_module_t *exp);
  void (*handle_event)(GdkEvent *event, gboolean blackwhite);
} dt_dev_proxy_exposure_t;

struct dt_dev_pixelpipe_t;
typedef struct dt_dev_viewport_t
{
  GtkWidget *widget;
  int32_t orig_width, orig_height;

  // dimensions of window
  int width, height;
  int32_t border_size;
  double dpi, dpi_factor, ppd;

  gboolean iso_12646;

  dt_dev_zoom_t zoom;
  int closeup;
  float zoom_x, zoom_y;
  float zoom_scale;

  // image processing pipeline with caching
  struct dt_dev_pixelpipe_t *pipe;
} dt_dev_viewport_t;

/* keep track on what and where we do chromatic adaptation, used
  a)  to display warnings in GUI of modules that should probably not be doing
      white balance
  b)  missing required white balance
  c)  allow late correction of as-shot or modified coeffs to D65 in color-input
      but keep processing until then with more reliable (at least for highlights,
      raw chromatic aberrations and more.
  d)  avoids keeping of fixed data in temperature gui data
  e)  we have 3 coefficients kept here
      - the currently used wb_coeffs in temperature module
      - D65coeffs and as_shot are read from exif data
  f)  - late_correction set by temperature if we want to process data as following
      If we use the new DT_IOP_TEMP_D65_LATE mode in temperature.c and don#t have
      any temp parameters changes later we can calc correction coeffs to modify
      as_shot rgb data to D65
*/
typedef struct dt_dev_chroma_t
{
  struct dt_iop_module_t *temperature;  // always available for GUI reports
  struct dt_iop_module_t *adaptation;   // set if one module is processing this without blending

  double wb_coeffs[4];                  // data actually used by the pipe
  double D65coeffs[4];                  // both read from exif data or "best guess"
  double as_shot[4];
  gboolean late_correction;
} dt_dev_chroma_t;

typedef struct dt_develop_t
{
  gboolean gui_attached; // != 0 if the gui should be notified of changes in hist stack and modules should be
                         // gui_init'ed.
  gboolean gui_leaving;  // set if everything is scheduled to shut down.
  gboolean gui_synch;    // set to TRUE by the render threads if gui_update should be called in the modules.

  gpointer gui_previous_target; // widget that was changed last time. If same again, don't save undo.
  double   gui_previous_time;   // last time that widget was changed. If too recent, don't save undo.
  double   gui_previous_pipe_time; // time pipe finished after last widget was changed.

  gboolean focus_hash;   // determines whether to start a new history item or to merge down.
  gboolean history_updating, image_force_reload, first_load;
  gboolean autosaving;
  double autosave_time;
  int32_t image_invalid_cnt;
  uint32_t timestamp;
  uint32_t preview_average_delay;
  struct dt_iop_module_t *gui_module; // this module claims gui expose/event callbacks.

  // image processing pipeline with caching
  struct dt_dev_pixelpipe_t *preview_pipe;

  // image under consideration, which
  // is copied each time an image is changed. this means we have some information
  // always cached (might be out of sync, so stars are not reliable), but for the iops
  // it's quite a convenience to access trivial stuff which is constant anyways without
  // calling into the cache explicitly. this should never be accessed directly, but
  // by the iop through the copy their respective pixelpipe holds, for thread-safety.
  dt_image_t image_storage;
  dt_imgid_t requested_id;
  int32_t snapshot_id; /* for the darkroom snapshots */

  // history stack
  dt_pthread_mutex_t history_mutex;
  int32_t history_end;
  GList *history;
  // some modules don't want to add new history items while active
  gboolean history_postpone_invalidate;
  // avoid checking for latest added module into history via list traversal
  struct dt_iop_module_t *history_last_module;

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
  gboolean full_preview;
  dt_dev_zoom_t full_preview_last_zoom;
  int full_preview_last_closeup;
  float full_preview_last_zoom_x, full_preview_last_zoom_y;
  struct dt_iop_module_t *full_preview_last_module;
  int full_preview_masks_state;

  /* proxy for communication between plugins and develop/darkroom */
  struct
  {
    // list of exposure iop instances, with plugin hooks, used by
    // histogram dragging functions each element is
    // dt_dev_proxy_exposure_t
    dt_dev_proxy_exposure_t exposure;

    // this module receives right-drag events if not already claimed
    struct dt_iop_module_t *rotate;

    // modulegroups plugin hooks
    struct
    {
      struct dt_lib_module_t *module;
      /* switch module group */
      void (*set)(struct dt_lib_module_t *self,
                  const uint32_t group);
      /* get current module group */
      uint32_t (*get)(struct dt_lib_module_t *self);
      /* get activated module group */
      uint32_t (*get_activated)(struct dt_lib_module_t *self);
      /* test if iop group flags matches modulegroup */
      gboolean (*test)(struct dt_lib_module_t *self,
                       const uint32_t group,
                       struct dt_iop_module_t *module);
      /* switch to modulegroup */
      void (*switch_group)(struct dt_lib_module_t *self,
                           struct dt_iop_module_t *module);
      /* update modulegroup visibility */
      void (*update_visibility)(struct dt_lib_module_t *self);
      /* test if module is preset in one of the current groups */
      gboolean (*test_visible)(struct dt_lib_module_t *self,
                               gchar *module);
      /* add or remove module or widget in current quick access list */
      gboolean (*basics_module_toggle)(struct dt_lib_module_t *self,
                                       GtkWidget *widget,
                                       const gboolean doit);
    } modulegroups;

    // masks plugin hooks
    struct
    {
      struct dt_lib_module_t *module;
      /* treview list refresh */
      void (*list_change)(struct dt_lib_module_t *self);
      void (*list_remove)(struct dt_lib_module_t *self,
                          const dt_mask_id_t formid,
                          const dt_mask_id_t parentid);
      void (*list_update)(struct dt_lib_module_t *self);
      /* selected forms change */
      void (*selection_change)(struct dt_lib_module_t *self,
                               struct dt_iop_module_t *module,
                               const dt_mask_id_t selectid);
    } masks;
  } proxy;

  dt_dev_chroma_t chroma;

  // for exposing and handling the crop
  struct
  {
    // set by dt_dev_pixelpipe_synch() if an enabled crop module is included in history
    struct dt_iop_module_t *exposer;

    // proxy to change crop settings via flip module
    struct dt_iop_module_t *flip_handler;
    void (*flip_callback)(struct dt_iop_module_t *crop,
                          const dt_image_orientation_t flipmode);
  } cropping;

  // for the overexposure indicator
  struct
  {
    GtkWidget *floating_window, *button;
    // yes, having gtk stuff in here is ugly. live with it.

    gboolean enabled;
    dt_dev_overexposed_colorscheme_t colorscheme;
    float lower;
    float upper;
    dt_clipping_preview_mode_t mode;
  } overexposed;

  // for the raw overexposure indicator
  struct
  {
    GtkWidget *floating_window, *button;
    // yes, having gtk stuff in here is ugly. live with it.

    gboolean enabled;
    dt_dev_rawoverexposed_mode_t mode;
    dt_dev_rawoverexposed_colorscheme_t colorscheme;
    float threshold;
  } rawoverexposed;

  // ISO 12646-compliant colour assessment conditions
  struct
  {
    GtkWidget *button; // yes, ugliness is the norm. what did you expect ?
  } iso_12646;

  // late scaling down from full roi
  struct
  {
    GtkWidget *button;
    gboolean enabled;
  } late_scaling;

  // the display profile related things (softproof, gamut check, profiles ...)
  struct
  {
    GtkWidget *floating_window, *softproof_button, *gamut_button;
  } profile;

  GtkWidget *second_wnd, *second_wnd_button;

  // several views of the same image
  dt_dev_viewport_t full, preview2;

  int mask_form_selected_id; // select a mask inside an iop
  gboolean darkroom_skip_mouse_events; // skip mouse events for masks
  gboolean darkroom_mouse_in_center_area; // TRUE if the mouse cursor is in center area

  GList *module_filter_out;
} dt_develop_t;

void dt_dev_init(dt_develop_t *dev, gboolean gui_attached);
void dt_dev_cleanup(dt_develop_t *dev);

float dt_dev_get_preview_downsampling();
void dt_dev_process_image_job(dt_develop_t *dev,
                              dt_dev_viewport_t *port,
                              struct dt_dev_pixelpipe_t *pipe,
                              dt_signal_t signal,
                              const int devid);
// launch jobs above
void dt_dev_process_image(dt_develop_t *dev);
void dt_dev_process_preview(dt_develop_t *dev);
void dt_dev_process_preview2(dt_develop_t *dev);

void dt_dev_load_image(dt_develop_t *dev,
                       const dt_imgid_t imgid);
void dt_dev_reload_image(dt_develop_t *dev,
                         const dt_imgid_t imgid);
/** checks if provided imgid is the image currently in develop */
gboolean dt_dev_is_current_image(const dt_develop_t *dev,
                                 const dt_imgid_t imgid);
const dt_dev_history_item_t *dt_dev_get_history_item(dt_develop_t *dev,
                                                     const char *op);
void dt_dev_add_history_item_ext(dt_develop_t *dev,
                                 struct dt_iop_module_t *module,
                                 const gboolean enable,
                                 const gboolean no_image);
void dt_dev_add_history_item(dt_develop_t *dev,
                             struct dt_iop_module_t *module,
                             const gboolean enable);
void dt_dev_add_history_item_target(dt_develop_t *dev,
                                    struct dt_iop_module_t *module,
                                    const gboolean enable,
                                    const gpointer target);
void dt_dev_add_new_history_item(dt_develop_t *dev,
                                 struct dt_iop_module_t *module,
                                 const gboolean enable);
void dt_dev_add_masks_history_item_ext(dt_develop_t *dev,
                                       struct dt_iop_module_t *_module,
                                       const gboolean _enable,
                                       const gboolean no_image);
void dt_dev_add_masks_history_item(dt_develop_t *dev,
                                   struct dt_iop_module_t *_module,
                                   const gboolean enable);
void dt_dev_reload_history_items(dt_develop_t *dev);
void dt_dev_pop_history_items_ext(dt_develop_t *dev, int32_t cnt);
void dt_dev_pop_history_items(dt_develop_t *dev, int32_t cnt);
void dt_dev_write_history_ext(dt_develop_t *dev, const dt_imgid_t imgid);
void dt_dev_write_history(dt_develop_t *dev);
void dt_dev_read_history_ext(dt_develop_t *dev,
                             const dt_imgid_t imgid,
                             const gboolean no_image);
void dt_dev_read_history(dt_develop_t *dev);
void dt_dev_free_history_item(gpointer data);
void dt_dev_invalidate_history_module(GList *list,
                                      struct dt_iop_module_t *module);

void dt_dev_invalidate(dt_develop_t *dev);
// also invalidates preview (which is unaffected by resize/zoom/pan)
void dt_dev_invalidate_all(dt_develop_t *dev);
void dt_dev_set_histogram(dt_develop_t *dev);
void dt_dev_set_histogram_pre(dt_develop_t *dev);
void dt_dev_reprocess_all(dt_develop_t *dev);
void dt_dev_reprocess_center(dt_develop_t *dev);
void dt_dev_reprocess_preview(dt_develop_t *dev);

gboolean dt_dev_get_preview_size(const dt_develop_t *dev,
                                 float *wd,
                                 float *ht);
void dt_dev_get_processed_size(dt_dev_viewport_t *port,
                               int *procw,
                               int *proch);
gboolean dt_dev_get_zoom_bounds(dt_dev_viewport_t *port,
                                float *zoom_x,
                                float *zoom_y,
                                float *boxww,
                                float *boxhh);
void dt_dev_zoom_move(dt_dev_viewport_t *port,
                      dt_dev_zoom_t zoom,
                      float scale,
                      int closeup,
                      float x,
                      float y,
                      gboolean constrain);
float dt_dev_get_zoom_scale(dt_dev_viewport_t *port,
                            dt_dev_zoom_t zoom,
                            const int closeup_factor,
                            const int mode);
float dt_dev_get_zoom_scale_full(void);
float dt_dev_get_zoomed_in(void);
void dt_dev_get_pointer_zoom_pos(dt_dev_viewport_t *port,
                                 const float px,
                                 const float py,
                                 float *zoom_x,
                                 float *zoom_y,
                                 float *zoom_scale);
void dt_dev_get_pointer_zoom_pos_from_bounds(dt_dev_viewport_t *port,
                                             const float px,
                                             const float py,
                                             const float zbound_x,
                                             const float zbound_y,
                                             float *zoom_x,
                                             float *zoom_y,
                                             float *zoom_scale);
void dt_dev_get_viewport_params(dt_dev_viewport_t *port,
                                dt_dev_zoom_t *zoom,
                                int *closeup,
                                float *x,
                                float *y);

void dt_dev_configure(dt_dev_viewport_t *port);

/** get exposure level */
float dt_dev_exposure_get_exposure(dt_develop_t *dev);
/** get exposure black level */
float dt_dev_exposure_get_black(dt_develop_t *dev);

void dt_dev_exposure_handle_event(GdkEvent *event, gboolean blackwhite);

/*
 * modulegroups plugin hooks
 */
/** switch to modulegroup of module */
void dt_dev_modulegroups_switch(dt_develop_t *dev,
                                struct dt_iop_module_t *module);
/** update modulegroup visibility */
void dt_dev_modulegroups_update_visibility(dt_develop_t *dev);
/** set the active modulegroup */
void dt_dev_modulegroups_set(dt_develop_t *dev, uint32_t group);
/** get the active modulegroup */
uint32_t dt_dev_modulegroups_get(dt_develop_t *dev);
/** get the activated modulegroup */
uint32_t dt_dev_modulegroups_get_activated(dt_develop_t *dev);
/** tests for a proper modulgroup being activated */
gboolean dt_dev_modulegroups_test_activated(dt_develop_t *dev);
/** test if iop group flags matches modulegroup */
gboolean dt_dev_modulegroups_test(dt_develop_t *dev,
                                  const uint32_t group,
                                  struct dt_iop_module_t *module);
/** reorder the module list */
void dt_dev_reorder_gui_module_list(dt_develop_t *dev);
/** test if the iop is visible in current groups layout **/
gboolean dt_dev_modulegroups_is_visible(dt_develop_t *dev,
                                        gchar *module);
/** add or remove module or widget in current quick access list **/
int dt_dev_modulegroups_basics_module_toggle(dt_develop_t *dev,
                                             GtkWidget *widget,
                                             const gboolean doit);

/*
 * masks plugin hooks
 */
void dt_dev_masks_list_change(dt_develop_t *dev);
void dt_dev_masks_list_update(dt_develop_t *dev);
void dt_dev_masks_list_remove(dt_develop_t *dev,
                              const dt_mask_id_t formid,
                              const dt_mask_id_t parentid);
void dt_dev_masks_selection_change(dt_develop_t *dev,
                                   struct dt_iop_module_t *module,
                                   const dt_mask_id_t selectid);

/*
 * multi instances
 */
/** duplicate a existent module */
struct dt_iop_module_t *dt_dev_module_duplicate_ext(dt_develop_t *dev,
                                                    struct dt_iop_module_t *base,
                                                    const gboolean reorder_iop);
struct dt_iop_module_t *dt_dev_module_duplicate(dt_develop_t *dev,
                                                struct dt_iop_module_t *base);
/** remove an existent module */
void dt_dev_module_remove(dt_develop_t *dev,
                          struct dt_iop_module_t *module);
/** generates item multi-instance name */
gchar *dt_history_item_get_name(const struct dt_iop_module_t *module);

/*
 * distort functions
 */
/** apply all transforms to the specified points (in preview pipe space) */
gboolean dt_dev_distort_transform
  (dt_develop_t *dev,
   float *points,
   const size_t points_count);
/** reverse apply all transforms to the specified points (in preview pipe space) */
gboolean dt_dev_distort_backtransform
  (dt_develop_t *dev,
   float *points,
   const size_t points_count);
/** same fct, but we can specify iop with priority between pmin and pmax */
gboolean dt_dev_distort_transform_plus
  (dt_develop_t *dev,
   struct dt_dev_pixelpipe_t *pipe,
   const double iop_order,
   const dt_dev_transform_direction_t transf_direction,
   float *points,
   const size_t points_count);
/** same fct as dt_dev_distort_backtransform, but we can specify iop
 * with priority between pmin and pmax */
gboolean dt_dev_distort_backtransform_plus
  (dt_develop_t *dev,
   struct dt_dev_pixelpipe_t *pipe,
   const double iop_order,
   const dt_dev_transform_direction_t transf_direction,
   float *points,
   const size_t points_count);

/** get the iop_pixelpipe instance corresponding to the iop in the given pipe */
struct dt_dev_pixelpipe_iop_t *dt_dev_distort_get_iop_pipe(dt_develop_t *dev,
                                                           struct dt_dev_pixelpipe_t *pipe,
                                                           struct dt_iop_module_t *module);
/*
 * hash functions
 */
/** generate hash value out of all module settings of pixelpipe.
    We specify iop with priority either up to iop_order or above iop_order depending on
    transfer direction */
dt_hash_t dt_dev_hash_plus(dt_develop_t *dev,
                          struct dt_dev_pixelpipe_t *pipe,
                          const double iop_order,
                          const dt_dev_transform_direction_t transf_direction);
/** synchronize pixelpipe by means hash values by waiting with timeout
 * and potential reprocessing */
gboolean dt_dev_sync_pixelpipe_hash(dt_develop_t *dev,
                               struct dt_dev_pixelpipe_t *pipe,
                               const double iop_order,
                               const dt_dev_transform_direction_t transf_direction,
                               dt_pthread_mutex_t *lock,
                               const volatile dt_hash_t *const hash);
/** generate hash value out of module settings of all distorting modules of pixelpipe
    We can specify iop with priority between pmin and pmax */
dt_hash_t dt_dev_hash_distort_plus(dt_develop_t *dev,
                                  struct dt_dev_pixelpipe_t *pipe,
                                  const double iop_order,
                                  const dt_dev_transform_direction_t transf_direction);
/*
 *   history undo support helpers for darkroom
 */

/* all history change must be enclosed into a start / end call */
void dt_dev_undo_start_record(dt_develop_t *dev);
void dt_dev_undo_end_record(dt_develop_t *dev);

/*
 * develop an image and returns the buf and processed width / height.
 * this is done as in the context of the darkroom, meaning that the
 * final processed sizes will align perfectly on the darkroom view.
 * if called with a valid CL devid that device is used without locking/unlocking as the caller is doing that
 */
void dt_dev_image(const dt_imgid_t imgid,
                  const size_t width,
                  const size_t height,
                  const int history_end,
                  uint8_t **buf,
                  float *scale,
                  size_t *buf_width,
                  size_t *buf_height,
                  float *zoom_x,
                  float *zoom_y,
                  const int32_t snapshot_id,
                  GList *module_filter_out,
                  const int devid,
                  const gboolean finalscale);


gboolean dt_dev_equal_chroma(const float *f, const double *d);
gboolean dt_dev_is_D65_chroma(const dt_develop_t *dev);
void dt_dev_reset_chroma(dt_develop_t *dev);
void dt_dev_init_chroma(dt_develop_t *dev);
void dt_dev_clear_chroma_troubles(dt_develop_t *dev);
static inline struct dt_iop_module_t *dt_dev_gui_module(void)
{
  return darktable.develop ? darktable.develop->gui_module : NULL;
}

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
