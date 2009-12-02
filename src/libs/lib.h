#ifndef DT_LIB_H
#define DT_LIB_H

#include "common/darktable.h"
#include <gmodule.h>
#include <gtk/gtk.h>

struct dt_lib_module_t;
/** struct responsible for all library related shared routines and plugins. */
typedef struct dt_lib_t
{
  GList *plugins;
  struct dt_lib_module_t *gui_module;
}
dt_lib_t;

typedef struct dt_lib_module_t
{
  /** opened module. */
  GModule *module;
  /** order in which plugins are stacked. */
  int32_t priority;
  /** reference for dlopened libs. */
  darktable_t *dt;
  /** other stuff that may be needed by the module, not only in gui mode. */
  void *data;
  /** string identifying this operation. */
  char plugin_name[128];
  /** child widget which is added to the GtkExpander. */
  GtkWidget *widget;
  /** expander containing the widget. */
  GtkExpander *expander;

  /** get name of the module, to be translated. */
  const char* (*name)     ();
  /** callback methods for gui. */
  /** construct widget. */
  void (*gui_init)        (struct dt_lib_module_t *self);
  /** destroy widget. */
  void (*gui_cleanup)     (struct dt_lib_module_t *self);
  /** reset to defaults. */
  void (*gui_reset)       (struct dt_lib_module_t *self);

  /** optional event callbacks for big center widget. */
  /** optional method called after lighttable expose. */
  void (*gui_post_expose) (struct dt_lib_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery);
  int  (*mouse_leave)     (struct dt_lib_module_t *self);
  int  (*mouse_moved)     (struct dt_lib_module_t *self, double x, double y, int which);
  int  (*button_released) (struct dt_lib_module_t *self, double x, double y, int which, uint32_t state);
  int  (*button_pressed)  (struct dt_lib_module_t *self, double x, double y, int which, int type, uint32_t state);
  int  (*key_pressed)     (struct dt_lib_module_t *self, uint16_t which);
  int  (*scrolled)        (struct dt_lib_module_t *self, double x, double y, int up);
  void (*configure)       (struct dt_lib_module_t *self, int width, int height);
  
  // TODO: gui only?
  // void (*init) (struct dt_lib_module_t *self);
  // void (*cleanup) (struct dt_lib_module_t *self);
}
dt_lib_module_t;

void dt_lib_init(dt_lib_t *lib);
void dt_lib_cleanup(dt_lib_t *lib);

/** loads and inits the modules in the libs/ directory. */
int dt_lib_load_modules();
/** calls module->cleanup and closes the dl connection. */
void dt_lib_unload_module(dt_lib_module_t *module);
/** creates a label widget for the expander, with callback to enable/disable this module. */
GtkWidget *dt_lib_gui_get_expander(dt_lib_module_t *module);

#endif
