/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#ifndef DARKTABLE_DEVELOP_H
#define DARKTABLE_DEVELOP_H

#include <inttypes.h>
#include <cairo.h>
#include <glib.h>
#include <stdint.h>

#include "common/darktable.h"
#include "common/dtpthread.h"
#include "control/settings.h"
#include "develop/imageop.h"
#include "common/image.h"

struct dt_iop_module_t;

typedef struct dt_dev_history_item_t
{
  struct dt_iop_module_t *module; // pointer to image operation module
  int32_t enabled;                // switched respective module on/off
  dt_iop_params_t *params;        // parameters for this operation
  struct dt_develop_blend_params_t *blend_params;
  int multi_priority;
  char multi_name[128];
  int32_t focus_hash;             // used to determine whether or not to start a new item or to merge down
} dt_dev_history_item_t;

typedef enum dt_dev_overexposed_colorscheme_t
{
  DT_DEV_OVEREXPOSED_BLACKWHITE = 0,
  DT_DEV_OVEREXPOSED_REDBLUE = 1,
  DT_DEV_OVEREXPOSED_PURPLEGREEN = 2
} dt_dev_overexposed_colorscheme_t;

typedef enum dt_dev_histogram_type_t
{
  DT_DEV_HISTOGRAM_LOGARITHMIC = 0,
  DT_DEV_HISTOGRAM_LINEAR,
  DT_DEV_HISTOGRAM_WAVEFORM,
  DT_DEV_HISTOGRAM_N // needs to be the last one
} dt_dev_histogram_type_t;

typedef enum dt_dev_pixelpipe_status_t
{
  DT_DEV_PIXELPIPE_DIRTY = 0,   // history stack changed or image new
  DT_DEV_PIXELPIPE_RUNNING = 1, // pixelpipe is running
  DT_DEV_PIXELPIPE_VALID = 2,   // pixelpipe has finished; valid result
  DT_DEV_PIXELPIPE_INVALID = 3  // pixelpipe has finished; invalid result
} dt_dev_pixelpipe_status_t;

extern const gchar *dt_dev_histogram_type_names[];

struct dt_dev_pixelpipe_t;
typedef struct dt_develop_t
{
  int32_t gui_attached; // != 0 if the gui should be notified of changes in hist stack and modules should be
                        // gui_init'ed.
  int32_t gui_leaving;  // set if everything is scheduled to shut down.
  int32_t gui_synch;    // set by the render threads if gui_update should be called in the modules.
  int32_t focus_hash;   // determines whether to start a new history item or to merge down.
  int32_t image_loading, first_load, image_force_reload;
  int32_t preview_loading, preview_input_changed;
  dt_dev_pixelpipe_status_t image_status, preview_status;
  uint32_t timestamp;
  uint32_t average_delay;
  uint32_t preview_average_delay;
  struct dt_iop_module_t *gui_module; // this module claims gui expose/event callbacks.
  float preview_downsampling;         // < 1.0: optionally downsample preview

  // width, height: dimensions of window
  int32_t width, height;

  // image processing pipeline with caching
  struct dt_dev_pixelpipe_t *pipe, *preview_pipe;
  dt_pthread_mutex_t pipe_mutex, preview_pipe_mutex; // these are locked while the pipes are still in use

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

  // histogram for display.
  uint32_t *histogram, *histogram_pre_tonecurve, *histogram_pre_levels;
  uint32_t histogram_max, histogram_pre_tonecurve_max, histogram_pre_levels_max;
  uint32_t *histogram_waveform, histogram_waveform_width, histogram_waveform_height,
      histogram_waveform_stride;
  // we should process the waveform histogram in the correct size to make it not look like crap. since this
  // requires gui knowledge we need this mutex
  //   dt_pthread_mutex_t histogram_waveform_mutex;
  dt_dev_histogram_type_t histogram_type;

  // list of forms iop can use for masks or whatever
  GList *forms;
  struct dt_masks_form_t *form_visible;
  struct dt_masks_form_gui_t *form_gui;

  //full preview stuff
  int full_preview;
  int full_preview_last_zoom, full_preview_last_closeup;
  float full_preview_last_zoom_x, full_preview_last_zoom_y;
  struct dt_iop_module_t *full_preview_last_module;
  int full_preview_masks_state;

  /* proxy for communication between plugins and develop/darkroom */
  struct
  {
    // exposure plugin hooks, used by histogram dragging functions
    struct
    {
      struct dt_iop_module_t *module;
      void (*set_white)(struct dt_iop_module_t *exp, const float white);
      float (*get_white)(struct dt_iop_module_t *exp);
      void (*set_black)(struct dt_iop_module_t *exp, const float black);
      float (*get_black)(struct dt_iop_module_t *exp);
    } exposure;

    // modulegroups plugin hooks
    struct
    {
      struct dt_lib_module_t *module;
      /* switch module group */
      void (*set)(struct dt_lib_module_t *self, uint32_t group);
      /* get current module group */
      uint32_t (*get)(struct dt_lib_module_t *self);
      /* test if iop group flags matches modulegroup */
      gboolean (*test)(struct dt_lib_module_t *self, uint32_t group, uint32_t iop_group);
      /* switch to modulegroup */
      void (*switch_group)(struct dt_lib_module_t *self, struct dt_iop_module_t *module);
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
      void (*selection_change)(struct dt_lib_module_t *self, int selectid, int throw_event);
    } masks;

  } proxy;

  // for the overexposure indicator
  struct
  {
    guint timeout;
    gulong destroy_signal_handler;
    GtkWidget *floating_window, *button; // yes, having gtk stuff in here is ugly. live with it.

    gboolean enabled;
    dt_dev_overexposed_colorscheme_t colorscheme;
    float lower;
    float upper;
  } overexposed;
} dt_develop_t;

void dt_dev_init(dt_develop_t *dev, int32_t gui_attached);
void dt_dev_cleanup(dt_develop_t *dev);

void dt_dev_process_image_job(dt_develop_t *dev);
void dt_dev_process_preview_job(dt_develop_t *dev);
// launch jobs above
void dt_dev_process_image(dt_develop_t *dev);
void dt_dev_process_preview(dt_develop_t *dev);

void dt_dev_load_image(dt_develop_t *dev, const uint32_t imgid);
void dt_dev_reload_image(dt_develop_t *dev, const uint32_t imgid);
/** checks if provided imgid is the image currently in develop */
int dt_dev_is_current_image(dt_develop_t *dev, uint32_t imgid);
void dt_dev_add_history_item(dt_develop_t *dev, struct dt_iop_module_t *module, gboolean enable);
void dt_dev_reload_history_items(dt_develop_t *dev);
void dt_dev_pop_history_items(dt_develop_t *dev, int32_t cnt);
void dt_dev_write_history(dt_develop_t *dev);
void dt_dev_read_history(dt_develop_t *dev);

void dt_dev_invalidate(dt_develop_t *dev);
// also invalidates preview (which is unaffected by resize/zoom/pan)
void dt_dev_invalidate_all(dt_develop_t *dev);
void dt_dev_set_histogram(dt_develop_t *dev);
void dt_dev_set_histogram_pre(dt_develop_t *dev);
void dt_dev_get_history_item_label(dt_dev_history_item_t *hist, char *label, const int cnt);
void dt_dev_reprocess_all(dt_develop_t *dev);
void dt_dev_reprocess_center(dt_develop_t *dev);

void dt_dev_get_processed_size(const dt_develop_t *dev, int *procw, int *proch);
void dt_dev_check_zoom_bounds(dt_develop_t *dev, float *zoom_x, float *zoom_y, dt_dev_zoom_t zoom,
                              int closeup, float *boxw, float *boxh);
float dt_dev_get_zoom_scale(dt_develop_t *dev, dt_dev_zoom_t zoom, int closeup_factor, int mode);
void dt_dev_get_pointer_zoom_pos(dt_develop_t *dev, const float px, const float py, float *zoom_x,
                                 float *zoom_y);


void dt_dev_configure(dt_develop_t *dev, int wd, int ht);
void dt_dev_invalidate_from_gui(dt_develop_t *dev);

/*
 * exposure plugin hook, set the white level
 */

/** check if exposure iop hooks are available */
gboolean dt_dev_exposure_hooks_available(dt_develop_t *dev);
/** reset exposure to defaults */
void dt_dev_exposure_reset_defaults(dt_develop_t *dev);
/** set exposure white level */
void dt_dev_exposure_set_white(dt_develop_t *dev, const float white);
/** get exposure white level */
float dt_dev_exposure_get_white(dt_develop_t *dev);
/** set exposure black level */
void dt_dev_exposure_set_black(dt_develop_t *dev, const float black);
/** get exposure black level */
float dt_dev_exposure_get_black(dt_develop_t *dev);

/*
 * modulegroups plugin hooks
 */
/** check if modulegroups hooks are available */
gboolean dt_dev_modulegroups_available(dt_develop_t *dev);
/** switch to modulegroup of module */
void dt_dev_modulegroups_switch(dt_develop_t *dev, struct dt_iop_module_t *module);
/** set the active modulegroup */
void dt_dev_modulegroups_set(dt_develop_t *dev, uint32_t group);
/** get the active modulegroup */
uint32_t dt_dev_modulegroups_get(dt_develop_t *dev);
/** test if iop group flags matches modulegroup */
gboolean dt_dev_modulegroups_test(dt_develop_t *dev, uint32_t group, uint32_t iop_group);

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
void dt_dev_masks_selection_change(dt_develop_t *dev, int selectid, int throw_event);

/*
 * multi instances
 */
/** duplicate a existant module */
struct dt_iop_module_t *dt_dev_module_duplicate(dt_develop_t *dev, struct dt_iop_module_t *base, int priority);
/** remove an existant module */
void dt_dev_module_remove(dt_develop_t *dev, struct dt_iop_module_t *module);
/** update "show" values of the multi instance part (show_move, show_delete, ...) */
void dt_dev_module_update_multishow(dt_develop_t *dev, struct dt_iop_module_t *module);
/** same, but for all modules */
void dt_dev_modules_update_multishow(dt_develop_t *dev);
/** generates item multi-instance name */
gchar *dt_history_item_get_name(struct dt_iop_module_t *module);
gchar *dt_history_item_get_name_html(struct dt_iop_module_t *module);

/*
 * distort functions
 */
/** apply all transforms to the specified points (in preview pipe space) */
int dt_dev_distort_transform(dt_develop_t *dev, float *points, size_t points_count);
/** reverse apply all transforms to the specified points (in preview pipe space) */
int dt_dev_distort_backtransform(dt_develop_t *dev, float *points, size_t points_count);
/** same fct, but we can specify iop with priority between pmin and pmax */
int dt_dev_distort_transform_plus(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, int pmin, int pmax,
                                  float *points, size_t points_count);
int dt_dev_distort_backtransform_plus(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, int pmin, int pmax,
                                      float *points, size_t points_count);
/** get the iop_pixelpipe instance corresponding to the iop in the given pipe */
struct dt_dev_pixelpipe_iop_t *dt_dev_distort_get_iop_pipe(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe,
                                                           struct dt_iop_module_t *module);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
