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

#ifdef FULL_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include <cairo/cairo.h>
#include <glib.h>
#include <stdint.h>

struct dt_view_t;

/* early definition of modules to do type checking */

#pragma GCC visibility push(default)

#endif // FULL_API_H

OPTIONAL(const char *, name, const struct dt_view_t *self); // get translatable name
OPTIONAL(uint32_t, view, const struct dt_view_t *self); // get the view type
DEFAULT(uint32_t, flags, );                             // get flags of the view
OPTIONAL(void, init, struct dt_view_t *self);           // init *data
OPTIONAL(void, gui_init, struct dt_view_t *self);       // create gtk elements, called after libs are created
OPTIONAL(void, cleanup, struct dt_view_t *self);        // cleanup *data
OPTIONAL(void, expose, struct dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                       int32_t pointery);         // expose the module (gtk callback)
OPTIONAL(int, try_enter, struct dt_view_t *self); // test if enter can succeed.
OPTIONAL(void, enter, struct dt_view_t *self);    // mode entered, this module got focus. return non-null on failure.
OPTIONAL(void, leave, struct dt_view_t *self);    // mode left (is called after the new try_enter has succeeded).
OPTIONAL(void, reset, struct dt_view_t *self);    // reset default appearance

// event callbacks:
OPTIONAL(void, mouse_enter, struct dt_view_t *self);
OPTIONAL(void, mouse_leave, struct dt_view_t *self);
OPTIONAL(void, mouse_moved, struct dt_view_t *self, double x, double y, double pressure, int which);

OPTIONAL(int, button_released, struct dt_view_t *self, double x, double y, int which, uint32_t state);
OPTIONAL(int, button_pressed, struct dt_view_t *self, double x, double y, double pressure,
                              int which, int type, uint32_t state);
OPTIONAL(void, configure, struct dt_view_t *self, int width, int height);
OPTIONAL(void, scrolled, struct dt_view_t *self, double x, double y, int up, int state); // mouse scrolled in view
OPTIONAL(void, scrollbar_changed, struct dt_view_t *self, double x, double y); // scrollbars changed in view

// list of mouse actions
OPTIONAL(GSList *, mouse_actions, const struct dt_view_t *self);

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
