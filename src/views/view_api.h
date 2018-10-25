/*
    This file is part of darktable,
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
#include <glib.h>
#include <stdint.h>

struct dt_view_t;

/* early definition of modules to do type checking */

// !!! MUST BE KEPT IN SYNC WITH dt_view_t defined in src/views/view.h !!!

#pragma GCC visibility push(default)

const char *name(struct dt_view_t *self);    // get translatable name
uint32_t view(const struct dt_view_t *self); // get the view type
uint32_t flags();                            // get flags of the view
void init(struct dt_view_t *self);           // init *data
void gui_init(struct dt_view_t *self);       // create gtk elements, called after libs are created
void cleanup(struct dt_view_t *self);        // cleanup *data
void expose(struct dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
            int32_t pointery);         // expose the module (gtk callback)
int try_enter(struct dt_view_t *self); // test if enter can succeed.
void enter(struct dt_view_t *self);    // mode entered, this module got focus. return non-null on failure.
void leave(struct dt_view_t *self);    // mode left (is called after the new try_enter has succeeded).
void reset(struct dt_view_t *self);    // reset default appearance

// event callbacks:
void mouse_enter(struct dt_view_t *self);
void mouse_leave(struct dt_view_t *self);
void mouse_moved(struct dt_view_t *self, double x, double y, double pressure, int which);

int button_released(struct dt_view_t *self, double x, double y, int which, uint32_t state);
int button_pressed(struct dt_view_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state);
int key_pressed(struct dt_view_t *self, guint key, guint state);
int key_released(struct dt_view_t *self, guint key, guint state);
void configure(struct dt_view_t *self, int width, int height);
void scrolled(struct dt_view_t *self, double x, double y, int up, int state); // mouse scrolled in view
void scrollbar_changed(struct dt_view_t *self, double x, double y); // scrollbars changed in view

// keyboard accel callbacks
void init_key_accels(struct dt_view_t *self);
void connect_key_accels(struct dt_view_t *self);

#pragma GCC visibility pop

#ifdef __cplusplus
}
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
