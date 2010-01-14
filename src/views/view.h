#ifndef DT_VIEW_H
#define DT_VIEW_H

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
  const char *(*name)     (struct dt_view_t *self); // get translatable name
  void (*init)            (struct dt_view_t *self); // init *data
  void (*cleanup)         (struct dt_view_t *self); // cleanup *data
  void (*expose)          (struct dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery); // expose the module (gtk callback)
  int  (*try_enter)       (struct dt_view_t *self); // test if enter can succeed.
  void (*enter)           (struct dt_view_t *self); // mode entered, this module got focus. return non-null on failure.
  void (*leave)           (struct dt_view_t *self); // mode left (is called after the new try_enter has succeded).
  void (*reset)           (struct dt_view_t *self); // reset default appearance

  // event callbacks:
  void (*mouse_leave)     (struct dt_view_t *self);
  void (*mouse_moved)     (struct dt_view_t *self, double x, double y, int which);
  void (*button_released) (struct dt_view_t *self, double x, double y, int which, uint32_t state);
  void (*button_pressed)  (struct dt_view_t *self, double x, double y, int which, int type, uint32_t state);
  void (*key_pressed)     (struct dt_view_t *self, uint16_t which);
  void (*configure)       (struct dt_view_t *self, int width, int height);
  void (*scrolled)        (struct dt_view_t *self, double x, double y, int up);
}
dt_view_t;

#define DT_VIEW_MAX_MODULES 10
/**
 * holds all relevant data needed to manage the view
 * modules.
 */
typedef struct dt_view_manager_t
{
  dt_view_t view[DT_VIEW_MAX_MODULES];
  int32_t current_view, num_views;
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
void dt_view_manager_button_released (dt_view_manager_t *vm, double x, double y, int which, uint32_t state);
void dt_view_manager_button_pressed  (dt_view_manager_t *vm, double x, double y, int which, int type, uint32_t state);
void dt_view_manager_key_pressed     (dt_view_manager_t *vm, uint16_t which);
void dt_view_manager_configure       (dt_view_manager_t *vm, int width, int height);
void dt_view_manager_scrolled        (dt_view_manager_t *vm, double x, double y, int up);

/** load module to view managers list, if still space. return slot number on success. */
int dt_view_manager_load_module(dt_view_manager_t *vm, const char *mod);
/** load a view module */
int dt_view_load_module(dt_view_t *view, const char *module);
/** unload, cleanup */
void dt_view_unload_module(dt_view_t *view);


#endif
