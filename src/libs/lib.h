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
#include <glib.h>

#include "common/darktable.h"
#include "common/iop_profile.h"
#include "views/view.h"
#include "gui/presets.h"

#ifdef USE_LUA
#include "lua/call.h"
#include "lua/events.h"
#include "lua/lib.h"
#include "lua/modules.h"
#include "lua/types.h"
#endif

struct dt_lib_module_t;
struct dt_colorpicker_sample_t;

/** struct responsible for all library related shared routines and plugins. */
typedef struct dt_lib_t
{
  GList *plugins;
  struct dt_lib_module_t *gui_module;

  /** Proxy functions for communication with views */
  struct
  {
    /** Colorpicker plugin hooks */
    struct
    {
      struct dt_lib_module_t *module;
      struct dt_colorpicker_sample_t *primary_sample;
      struct dt_iop_color_picker_t *picker_proxy;
      GSList *live_samples;
      struct dt_colorpicker_sample_t *selected_sample;
      gboolean display_samples;
      gboolean restrict_histogram;
      int statistic;
      void (*update_panel)(struct dt_lib_module_t *self);
      void (*update_samples)(struct dt_lib_module_t *self);
      void (*set_sample_box_area)(struct dt_lib_module_t *self,
                                  const dt_boundingbox_t size);
      void (*set_sample_point)(struct dt_lib_module_t *self,
                               const float pos[2]);
      void (*setup_sample)(struct dt_lib_module_t *self,
                           const gboolean denoise,
                           const gboolean pick_output);
    } colorpicker;

    /** Histogram processing hooks */
    struct
    {
      struct dt_lib_module_t *module;
      void (*process)(struct dt_lib_module_t *self, const float *const input,
                      int width, int height,
                      const dt_iop_order_iccprofile_info_t *const profile_info_from,
                      const dt_iop_order_iccprofile_info_t *const profile_info_to);
      // FIXME: now that PR #5532 is merged, define this as
      // dt_atomic_int and include "common/atomic.h" and use
      // dt_atomic_set_int() and dt_atomic_get_int()
      gboolean is_linear;
    } histogram;

    struct
    {
      struct dt_lib_module_t *module;
    } navigation;
  } proxy;
} dt_lib_t;

typedef struct dt_lib_module_t
{
  dt_action_t actions; // !!! NEEDS to be FIRST (to be able to cast convert)

#define INCLUDE_API_FROM_MODULE_H
#include "libs/lib_api.h"

  /** opened module. */
  GModule *module;
  /** other stuff that may be needed by the module, not only in gui mode. */
  void *data;
  /** string identifying this operation. */
  char plugin_name[128];
  /** child widget which is added to the GtkExpander. */
  GtkWidget *widget;
  /** expander containing the widget. */
  GtkWidget *expander;
  /** does gui need to be updated if visible */
  gboolean gui_uptodate;

  GtkWidget *arrow;
  GtkWidget *reset_button;
  GtkWidget *presets_button;

  gboolean pref_based_presets;
} dt_lib_module_t;

void dt_lib_init(dt_lib_t *lib);
void dt_lib_cleanup(dt_lib_t *lib);

/** creates a label widget for the expander, with callback to enable/disable this module. */
GtkWidget *dt_lib_gui_get_expander(dt_lib_module_t *module);
/** set an expand/collapse plugin expander */
void dt_lib_gui_set_expanded(dt_lib_module_t *module,
                             const gboolean expanded);
/** get the expanded state of a plugin */
gboolean dt_lib_gui_get_expanded(dt_lib_module_t *module);
/** queue plugin gui to be updated if visible */
void dt_lib_gui_queue_update(dt_lib_module_t *module);

extern const struct dt_action_def_t dt_action_def_lib;

/** return the plugin with the given name */
dt_lib_module_t *dt_lib_get_module(const char *name);

/** get the visible state of a plugin */
gboolean dt_lib_is_visible(dt_lib_module_t *module);
/** set the visible state of a plugin */
void dt_lib_set_visible(dt_lib_module_t *module,
                        const gboolean visible);
/** check if a plugin is to be shown in a given view */
gboolean dt_lib_is_visible_in_view(dt_lib_module_t *module,
                                   const dt_view_t *view);

/** returns the localized plugin name for a given plugin_name. must not be freed. */
gchar *dt_lib_get_localized_name(const gchar *plugin_name);

/** preset stuff for lib */

/** add or replace a preset for this operation. */
void dt_lib_presets_add(const char *name,
                        const char *plugin_name,
                        const int32_t version,
                        const void *params,
                        const int32_t params_size,
                        const gboolean readonly,
                        const dt_gui_presets_format_flag_t format);

// apply a preset to the given module
gboolean dt_lib_presets_apply(const gchar *preset,
                              const gchar *module_name,
                              const int module_version);
// duplicate a preset
gchar *dt_lib_presets_duplicate(const gchar *preset,
                                const gchar *module_name,
                                const int module_version);
// remove a preset
void dt_lib_presets_remove(const gchar *preset,
                           const gchar *module_name,
                           const int module_version);
// update a preset
void dt_lib_presets_update(const gchar *preset,
                           const gchar *module_name,
                           const int module_version,
                           const gchar *newname,
                           const gchar *desc,
                           const void *params,
                           const int32_t params_size);
// know if the module can autoapply presets
gboolean dt_lib_presets_can_autoapply(dt_lib_module_t *mod);

/*
 * Proxy functions
 */

/** set the colorpicker area selection tool and size, box[k] 0.0 - 1.0 */
void dt_lib_colorpicker_set_box_area(dt_lib_t *lib,
                                     const dt_boundingbox_t box);

/** set the colorpicker point selection tool and position */
void dt_lib_colorpicker_set_point(dt_lib_t *lib,
                                  const float pos[2]);

/** setup colorpicker options */
void dt_lib_colorpicker_setup(dt_lib_t *lib,
                              const gboolean denoise,
                              const gboolean pick_output);

/** sorter callback to add a lib in the list of libs after init */
gint dt_lib_sort_plugins(gconstpointer a, gconstpointer b);
/** init presets for a newly created lib */
void dt_lib_init_presets(dt_lib_module_t *module);

/** handle Enter key for dialog. Note it uses GTK_RESPONSE_ACCEPT code */
gboolean dt_handle_dialog_enter(GtkWidget *widget,
                                GdkEventKey *event,
                                gpointer data);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
