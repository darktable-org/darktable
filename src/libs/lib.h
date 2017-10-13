/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2011 henrik andersson.

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

#include "common/darktable.h"
#include "views/view.h"
#include <gmodule.h>
#include <gtk/gtk.h>
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
      uint8_t *picked_color_rgb_mean;
      uint8_t *picked_color_rgb_min;
      uint8_t *picked_color_rgb_max;
      float *picked_color_lab_mean;
      float *picked_color_lab_min;
      float *picked_color_lab_max;
      GSList *live_samples;
      struct dt_colorpicker_sample_t *selected_sample;
      int size;
      int display_samples;
      int restrict_histogram;
      void (*update_panel)(struct dt_lib_module_t *self);
      void (*update_samples)(struct dt_lib_module_t *self);
      void (*set_sample_area)(struct dt_lib_module_t *self, float size);
      void (*set_sample_point)(struct dt_lib_module_t *self, float x, float y);
    } colorpicker;

  } proxy;
} dt_lib_t;

typedef struct dt_lib_module_t
{
  // !!! MUST BE KEPT IN SYNC WITH src/libs/lib_api.h !!!

  /** opened module. */
  GModule *module;
  /** reference for dlopened libs. */
  darktable_t *dt;
  /** other stuff that may be needed by the module, not only in gui mode. */
  void *data;
  /** string identifying this operation. */
  char plugin_name[128];
  /** child widget which is added to the GtkExpander. */
  GtkWidget *widget;
  /** expander containing the widget. */
  GtkWidget *expander;

  /** version */
  int (*version)();
  /** get name of the module, to be translated. */
  const char *(*name)(struct dt_lib_module_t *self);
  /** get the views which the module should be loaded in. */
  const char **(*views)(struct dt_lib_module_t *self);
  /** get the container which the module should be placed in */
  uint32_t (*container)(struct dt_lib_module_t *self);
  /** check if module should use a expander or not, default implementation
      will make the module expandable and storing the expanding state,
      if not the module will always be shown without the expander. */
  int (*expandable)(struct dt_lib_module_t *self);

  /** constructor */
  void (*init)(struct dt_lib_module_t *self);
  /** callback methods for gui. */
  /** construct widget. */
  void (*gui_init)(struct dt_lib_module_t *self);
  /** destroy widget. */
  void (*gui_cleanup)(struct dt_lib_module_t *self);
  /** reset to defaults. */
  void (*gui_reset)(struct dt_lib_module_t *self);

  /** entering a view, only called if lib is displayed on the new view */
  void (*view_enter)(struct dt_lib_module_t *self,struct dt_view_t *old_view,struct dt_view_t *new_view);
  /** entering a view, only called if lib is displayed on the old view */
  void (*view_leave)(struct dt_lib_module_t *self,struct dt_view_t *old_view,struct dt_view_t *new_view);

  /** optional event callbacks for big center widget. */
  /** optional method called after lighttable expose. */
  void (*gui_post_expose)(struct dt_lib_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                          int32_t pointerx, int32_t pointery);
  int (*mouse_leave)(struct dt_lib_module_t *self);
  int (*mouse_moved)(struct dt_lib_module_t *self, double x, double y, double pressure, int which);
  int (*button_released)(struct dt_lib_module_t *self, double x, double y, int which, uint32_t state);
  int (*button_pressed)(struct dt_lib_module_t *self, double x, double y, double pressure, int which,
                        int type, uint32_t state);
  int (*scrolled)(struct dt_lib_module_t *self, double x, double y, int up);
  void (*configure)(struct dt_lib_module_t *self, int width, int height);
  int (*position)(const struct dt_lib_module_t *self);
  /** implement these three if you want customizable presets to be stored in db. */
  /** legacy_params can run in iterations, just return to what version you updated the preset. */
  void *(*legacy_params)(struct dt_lib_module_t *self, const void *const old_params,
                         const size_t old_params_size, const int old_version, int *new_version, size_t *new_size);
  void *(*get_params)(struct dt_lib_module_t *self, int *size);
  int (*set_params)(struct dt_lib_module_t *self, const void *params, int size);
  void (*init_presets)(struct dt_lib_module_t *self);
  /** Optional callbacks for keyboard accelerators */
  void (*init_key_accels)(struct dt_lib_module_t *self);
  void (*connect_key_accels)(struct dt_lib_module_t *self);

  GSList *accel_closures;
  GtkWidget *reset_button;
  GtkWidget *presets_button;
} dt_lib_module_t;

void dt_lib_init(dt_lib_t *lib);
void dt_lib_cleanup(dt_lib_t *lib);

/** creates a label widget for the expander, with callback to enable/disable this module. */
GtkWidget *dt_lib_gui_get_expander(dt_lib_module_t *module);
/** set a expand/collaps plugin expander */
void dt_lib_gui_set_expanded(dt_lib_module_t *module, gboolean expanded);
/** get the expanded state of a plugin */
gboolean dt_lib_gui_get_expanded(dt_lib_module_t *module);

/** connects the reset and presets shortcuts to a lib */
void dt_lib_connect_common_accels(dt_lib_module_t *module);

/** get the visible state of a plugin */
gboolean dt_lib_is_visible(dt_lib_module_t *module);
/** set the visible state of a plugin */
void dt_lib_set_visible(dt_lib_module_t *module, gboolean visible);
/** check if a plugin is to be shown in a given view */
gboolean dt_lib_is_visible_in_view(dt_lib_module_t *module, const dt_view_t *view);

/** returns the localized plugin name for a given plugin_name. must not be freed. */
gchar *dt_lib_get_localized_name(const gchar *plugin_name);

/** preset stuff for lib */

/** add or replace a preset for this operation. */
void dt_lib_presets_add(const char *name, const char *plugin_name, const int32_t version, const void *params,
                        const int32_t params_size);

/*
 * Proxy functions
 */

/** set the colorpicker area selection tool and size, size 0.0 - 1.0 */
void dt_lib_colorpicker_set_area(dt_lib_t *lib, float size);

/** set the colorpicker point selection tool and position */
void dt_lib_colorpicker_set_point(dt_lib_t *lib, float x, float y);

/** sorter callback to add a lib in the list of libs after init */
gint dt_lib_sort_plugins(gconstpointer a, gconstpointer b);
/** init presets for a newly created lib */
void dt_lib_init_presets(dt_lib_module_t *module);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
