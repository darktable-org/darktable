/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#ifndef DT_VIEW_H
#define DT_VIEW_H

#include "common/image.h"
#include <inttypes.h>
#include <gmodule.h>
#include <cairo.h>

/**
 * main dt view module (as lighttable or darkroom)
 */
struct dt_view_t;
typedef struct dt_view_t
{
  char module_name[64];
  // dlopened module
  GModule *module;
  // custom data for module
  void *data;
  // width and height of allocation
  uint32_t width, height;
  // scroll bar control
  float vscroll_size, vscroll_viewport_size, vscroll_pos;
  float hscroll_size, hscroll_viewport_size, hscroll_pos;
  const char *(*name)     (struct dt_view_t *self); // get translatable name
  void (*init)            (struct dt_view_t *self); // init *data
  void (*cleanup)         (struct dt_view_t *self); // cleanup *data
  void (*expose)          (struct dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery); // expose the module (gtk callback)
  int  (*try_enter)       (struct dt_view_t *self); // test if enter can succeed.
  void (*enter)           (struct dt_view_t *self); // mode entered, this module got focus. return non-null on failure.
  void (*leave)           (struct dt_view_t *self); // mode left (is called after the new try_enter has succeded).
  void (*reset)           (struct dt_view_t *self); // reset default appearance

  // event callbacks:
  int  (*mouse_leave)     (struct dt_view_t *self);
  int  (*mouse_moved)     (struct dt_view_t *self, double x, double y, int which);
  int  (*button_released) (struct dt_view_t *self, double x, double y, int which, uint32_t state);
  int  (*button_pressed)  (struct dt_view_t *self, double x, double y, int which, int type, uint32_t state);
  int  (*key_pressed)     (struct dt_view_t *self, uint16_t which);
  void (*configure)       (struct dt_view_t *self, int width, int height);
  void (*scrolled)        (struct dt_view_t *self, double x, double y, int up);            // mouse scrolled in view
  void (*border_scrolled) (struct dt_view_t *self, double x, double y, int which, int up); // mouse scrolled on left/right/top/bottom border (which 0123).
}
dt_view_t;

typedef enum dt_view_image_over_t
{
  DT_VIEW_DESERT = 0,
  DT_VIEW_STAR_1 = 1,
  DT_VIEW_STAR_2 = 2,
  DT_VIEW_STAR_3 = 3,
  DT_VIEW_STAR_4 = 4
}
dt_view_image_over_t;

/** expose an image, set image over flags. */
void dt_view_image_expose(dt_image_t *img, dt_view_image_over_t *image_over, int32_t index, cairo_t *cr, int32_t width, int32_t height, int32_t zoom, int32_t px, int32_t py);

/** toggle selection of given image. */
void dt_view_toggle_selection(int iid);


#define DT_VIEW_MAX_MODULES 10
/**
 * holds all relevant data needed to manage the view
 * modules.
 */
typedef struct dt_view_manager_t
{
  dt_view_t film_strip;
  dt_view_t view[DT_VIEW_MAX_MODULES];
  int32_t current_view, num_views;
  int32_t film_strip_on;
  float film_strip_size;
  int32_t film_strip_dragging, film_strip_scroll_to;
  void (*film_strip_activated)(const int imgid, void *data);
  void *film_strip_data;
}
dt_view_manager_t;

void dt_view_manager_init(dt_view_manager_t *vm);
void dt_view_manager_cleanup(dt_view_manager_t *vm);

/** return translated name. */
const char *dt_view_manager_name (dt_view_manager_t *vm);
/** switch to this module. returns non-null if the module fails to change. */
int dt_view_manager_switch(dt_view_manager_t *vm, int k);
/** expose current module. */
void dt_view_manager_expose(dt_view_manager_t *vm, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery);
/** reset current view. */
void dt_view_manager_reset(dt_view_manager_t *vm);

void dt_view_manager_mouse_leave     (dt_view_manager_t *vm);
void dt_view_manager_mouse_moved     (dt_view_manager_t *vm, double x, double y, int which);
int dt_view_manager_button_released (dt_view_manager_t *vm, double x, double y, int which, uint32_t state);
int dt_view_manager_button_pressed  (dt_view_manager_t *vm, double x, double y, int which, int type, uint32_t state);
int dt_view_manager_key_pressed     (dt_view_manager_t *vm, uint16_t which);
void dt_view_manager_configure       (dt_view_manager_t *vm, int width, int height);
void dt_view_manager_scrolled        (dt_view_manager_t *vm, double x, double y, int up);
void dt_view_manager_border_scrolled (dt_view_manager_t *vm, double x, double y, int which, int up);

/** load module to view managers list, if still space. return slot number on success. */
int dt_view_manager_load_module(dt_view_manager_t *vm, const char *mod);
/** load a view module */
int dt_view_load_module(dt_view_t *view, const char *module);
/** unload, cleanup */
void dt_view_unload_module(dt_view_t *view);
/** set scrollbar positions, gui method. */
void dt_view_set_scrollbar(dt_view_t *view, float hpos, float hsize, float hwinsize, float vpos, float vsize, float vwinsize);
/** open up the film strip view, with given callback on image activation. */
void dt_view_film_strip_open(dt_view_manager_t *vm, void (*activated)(const int, void*), void *data);
/** close the film strip view. */
void dt_view_film_strip_close(dt_view_manager_t *vm);
/** advise the film strip to scroll to imgid at next expose. */
void dt_view_film_strip_scroll_to(dt_view_manager_t *vm, const int imgid);

#endif
