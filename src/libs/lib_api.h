/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2011 henrik andersson.
    copyright (c) 2016 Roman Lebedev.

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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <cairo/cairo.h>
#include <stddef.h>
#include <stdint.h>

struct dt_lib_module_t;
struct dt_view_t;

/* early definition of modules to do type checking */

// !!! MUST BE KEPT IN SYNC WITH dt_lib_module_t defined in src/libs/lib.h !!!

#pragma GCC visibility push(default)

/** version */
int version();
/** get name of the module, to be translated. */
const char *name(struct dt_lib_module_t *self);

/** get the views which the module should be loaded in. */
const char **views(struct dt_lib_module_t *self);
/** get the container which the module should be placed in */
uint32_t container(struct dt_lib_module_t *self);
/** check if module should use a expander or not, default implementation
    will make the module expandable and storing the expanding state,
    if not the module will always be shown without the expander. */
int expandable(struct dt_lib_module_t *self);

/** constructor */
void init(struct dt_lib_module_t *self);
/** callback methods for gui. */
/** construct widget. */
void gui_init(struct dt_lib_module_t *self);
/** destroy widget. */
void gui_cleanup(struct dt_lib_module_t *self);
/** reset to defaults. */
void gui_reset(struct dt_lib_module_t *self);

/** entering a view, only called if lib is displayed on the new view */
void view_enter(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view);
/** entering a view, only called if lib is displayed on the old view */
void view_leave(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view);

/** optional event callbacks for big center widget. */
/** optional method called after lighttable expose. */
void gui_post_expose(struct dt_lib_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                     int32_t pointerx, int32_t pointery);
int mouse_leave(struct dt_lib_module_t *self);
int mouse_moved(struct dt_lib_module_t *self, double x, double y, double pressure, int which);
int button_released(struct dt_lib_module_t *self, double x, double y, int which, uint32_t state);
int button_pressed(struct dt_lib_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state);
int scrolled(struct dt_lib_module_t *self, double x, double y, int up);
void configure(struct dt_lib_module_t *self, int width, int height);
int position();

/** implement these three if you want customizable presets to be stored in db. */
/** legacy_params can run in iterations, just return to what version you updated the preset. */
void *legacy_params(struct dt_lib_module_t *self, const void *const old_params, const size_t old_params_size,
                    const int old_version, int *new_version, size_t *new_size);
void *get_params(struct dt_lib_module_t *self, int *size);
int set_params(struct dt_lib_module_t *self, const void *params, int size);
void init_presets(struct dt_lib_module_t *self);

/** Optional callbacks for keyboard accelerators */
void init_key_accels(struct dt_lib_module_t *self);
void connect_key_accels(struct dt_lib_module_t *self);

#pragma GCC visibility pop

#ifdef __cplusplus
}
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
