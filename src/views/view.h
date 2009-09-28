#ifndef DT_VIEW_H
#define DT_VIEW_H

#include <gmodule.h>
#include <cairo.h>

/**
 * main dt view module (as lighttable or darkroom)
 */
struct dt_view_t;
typedef struct dt_view_t
{
  // dlopened module
  GModule *module;
  // custom data for module
  void *data;
  void (*init)    (struct dt_view_t self); // init *data
  void (*cleanup) (struct dt_view_t self); // cleanup *data
  void (*expose)  (struct dt_view_t self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery); // expose the module (gtk callback)
  void (*enter)   (struct dt_view_t self); // mode entered, this module got focus
  void (*leave)   (struct dt_view_t self); // mode left.
}
dt_view_t;

/** load a view module */
void dt_view_load_module(dt_view_t *view, const char *module);
/** unload, cleanup */
void dt_view_unload_module(dt_view_t *view);

#endif
