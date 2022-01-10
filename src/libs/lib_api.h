/*
    This file is part of darktable,
    Copyright (C) 2016-2021 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/module_api.h"
#include <glib.h>

#ifdef FULL_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include <glib.h>
#include <cairo/cairo.h>
#include <stddef.h>
#include <stdint.h>

struct dt_lib_module_t;
struct dt_view_t;

/* early definition of modules to do type checking */

#pragma GCC visibility push(default)

#endif // FULL_API_H

/** get name of the module, to be translated. */
REQUIRED(const char *, name, struct dt_lib_module_t *self);

/** get the views which the module should be loaded in. */
REQUIRED(enum dt_view_type_flags_t, views, struct dt_lib_module_t *self);
/** get the container which the module should be placed in */
REQUIRED(uint32_t, container, struct dt_lib_module_t *self);
/** check if module should use a expander or not, default implementation
    will make the module expandable and storing the expanding state,
    if not the module will always be shown without the expander. */
DEFAULT(gboolean, expandable, struct dt_lib_module_t *self);

/** constructor */
OPTIONAL(void, init, struct dt_lib_module_t *self);
/** callback methods for gui. */
/** construct widget. */
REQUIRED(void, gui_init, struct dt_lib_module_t *self);
/** destroy widget. */
REQUIRED(void, gui_cleanup, struct dt_lib_module_t *self);
/** reset to defaults. */
OPTIONAL(void, gui_reset, struct dt_lib_module_t *self);
/** update libs gui when visible
    triggered by dt_lib_gui_queue_update.
    don't use for widgets accessible via actions when hidden. */
OPTIONAL(void, gui_update, struct dt_lib_module_t *self);

/** entering a view, only called if lib is displayed on the new view */
OPTIONAL(void, view_enter, struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view);
/** entering a view, only called if lib is displayed on the old view */
OPTIONAL(void, view_leave, struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view);

/** optional event callbacks for big center widget. */
/** optional method called after lighttable expose. */
OPTIONAL(void, gui_post_expose, struct dt_lib_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                     int32_t pointerx, int32_t pointery);
OPTIONAL(int, mouse_leave, struct dt_lib_module_t *self);
OPTIONAL(int, mouse_moved, struct dt_lib_module_t *self, double x, double y, double pressure, int which);
OPTIONAL(int, button_released, struct dt_lib_module_t *self, double x, double y, int which, uint32_t state);
OPTIONAL(int, button_pressed, struct dt_lib_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state);
OPTIONAL(int, scrolled, struct dt_lib_module_t *self, double x, double y, int up);
OPTIONAL(void, configure, struct dt_lib_module_t *self, int width, int height);
OPTIONAL(int, position, const struct dt_lib_module_t *self);

/** implement these three if you want customizable presets to be stored in db. */
/** legacy_params can run in iterations, just return to what version you updated the preset. */
OPTIONAL(void *,legacy_params, struct dt_lib_module_t *self, const void *const old_params, const size_t old_params_size,
                    const int old_version, int *new_version, size_t *new_size);
OPTIONAL(void *,get_params, struct dt_lib_module_t *self, int *size);
OPTIONAL(int, set_params, struct dt_lib_module_t *self, const void *params, int size);
OPTIONAL(void, init_presets, struct dt_lib_module_t *self);
OPTIONAL(void, manage_presets, struct dt_lib_module_t *self);
OPTIONAL(void, set_preferences, void *menu, struct dt_lib_module_t *self);
/** check if the module can autoapply presets. Default is FALSE */
DEFAULT(gboolean, preset_autoapply, struct dt_lib_module_t *self);

#ifdef FULL_API_H

#pragma GCC visibility pop

#ifdef __cplusplus
}
#endif

#endif // FULL_API_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
