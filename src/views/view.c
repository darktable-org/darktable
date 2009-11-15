
#include "common/darktable.h"
#include "develop/develop.h"
#include "views/view.h"
#include <stdlib.h>
#include <stdio.h>
#include <glib.h>
#include <string.h>
#include <strings.h>

void dt_view_manager_init(dt_view_manager_t *vm)
{
  vm->num_views = 0;
  // TODO: load all in directory?
  int k = -1;
  k = dt_view_manager_load_module(vm, "darkroom");
  // FIXME: this is global for plugins etc.
  if(k >= 0) darktable.develop = (dt_develop_t *)vm->view[k].data;
  vm->current_view = dt_view_manager_load_module(vm, "lighttable");
}

void dt_view_manager_cleanup(dt_view_manager_t *vm)
{
  for(int k=0;k<vm->num_views;k++) dt_view_unload_module(vm->view + k);
}

int dt_view_manager_load_module(dt_view_manager_t *vm, const char *mod)
{
  if(vm->num_views >= DT_VIEW_MAX_MODULES) return -1;
  if(dt_view_load_module(vm->view+vm->num_views, mod)) return -1;
  return vm->num_views++;
}

/** load a view module */
int dt_view_load_module(dt_view_t *view, const char *module)
{
  bzero(view, sizeof(dt_view_t));
  view->data = NULL;
  strncpy(view->module_name, module, 64);
  char datadir[1024];
  dt_get_datadir(datadir, 1024);
  strcpy(datadir + strlen(datadir), "/views");
  gchar *libname = g_module_build_path(datadir, (const gchar *)module);
  view->module = g_module_open(libname, G_MODULE_BIND_LAZY);
  g_free(libname);
  if(!view->module)
  {
    fprintf(stderr, "[view_load_module] could not open %s (%s)!\n", libname, g_module_error());
    return -1;
  }

  if(!g_module_symbol(view->module, "name",            (gpointer)&(view->name)))            view->name = NULL;
  if(!g_module_symbol(view->module, "init",            (gpointer)&(view->init)))            view->init = NULL;
  if(!g_module_symbol(view->module, "cleanup",         (gpointer)&(view->cleanup)))         view->cleanup = NULL;
  if(!g_module_symbol(view->module, "expose",          (gpointer)&(view->expose)))          view->expose = NULL;
  if(!g_module_symbol(view->module, "enter",           (gpointer)&(view->enter)))           view->enter = NULL;
  if(!g_module_symbol(view->module, "leave",           (gpointer)&(view->leave)))           view->leave = NULL;
  if(!g_module_symbol(view->module, "reset",           (gpointer)&(view->reset)))           view->reset = NULL;
  if(!g_module_symbol(view->module, "mouse_leave",     (gpointer)&(view->mouse_leave)))     view->mouse_leave = NULL;
  if(!g_module_symbol(view->module, "mouse_moved",     (gpointer)&(view->mouse_moved)))     view->mouse_moved = NULL;
  if(!g_module_symbol(view->module, "button_released", (gpointer)&(view->button_released))) view->button_released = NULL;
  if(!g_module_symbol(view->module, "button_pressed",  (gpointer)&(view->button_pressed)))  view->button_pressed = NULL;
  if(!g_module_symbol(view->module, "key_pressed",     (gpointer)&(view->key_pressed)))     view->key_pressed = NULL;
  if(!g_module_symbol(view->module, "configure",       (gpointer)&(view->configure)))       view->configure = NULL;
  if(!g_module_symbol(view->module, "scrolled",        (gpointer)&(view->scrolled)))        view->scrolled = NULL;

  if(view->init) view->init(view);
  return 0;
}

/** unload, cleanup */
void dt_view_unload_module (dt_view_t *view)
{
  if(view->cleanup) view->cleanup(view);
  if(view->module) g_module_close(view->module);
}

void dt_view_manager_switch (dt_view_manager_t *vm, int k)
{
  dt_view_t *v = vm->view + vm->current_view;
  if(v->leave) v->leave(v);
  if(k < DT_VIEW_MAX_MODULES && k >= 0) vm->current_view = k;
  v = vm->view + vm->current_view;
  if(v->enter) v->enter(v);
}

const char *dt_view_manager_name (dt_view_manager_t *vm)
{
  dt_view_t *v = vm->view + vm->current_view;
  if(v->name) return v->name(v);
  else return v->module_name;
}

void dt_view_manager_expose (dt_view_manager_t *vm, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_view_t *v = vm->view + vm->current_view;
  if(v->expose) v->expose(v, cr, width, height, pointerx, pointery);
}

void dt_view_manager_reset (dt_view_manager_t *vm)
{
  dt_view_t *v = vm->view + vm->current_view;
  if(v->reset) v->reset(v);
}

void dt_view_manager_mouse_leave (dt_view_manager_t *vm)
{
  dt_view_t *v = vm->view + vm->current_view;
  if(v->mouse_leave) v->mouse_leave(v);
}

void dt_view_manager_mouse_moved (dt_view_manager_t *vm, double x, double y, int which)
{
  dt_view_t *v = vm->view + vm->current_view;
  if(v->mouse_moved) v->mouse_moved(v, x, y, which);
}

void dt_view_manager_button_released (dt_view_manager_t *vm, double x, double y, int which, uint32_t state)
{
  dt_view_t *v = vm->view + vm->current_view;
  if(v->button_released) v->button_released(v, x, y, which, state);
}

void dt_view_manager_button_pressed (dt_view_manager_t *vm, double x, double y, int which, int type, uint32_t state)
{
  dt_view_t *v = vm->view + vm->current_view;
  if(v->button_pressed) v->button_pressed(v, x, y, which, type, state);
}

void dt_view_manager_key_pressed (dt_view_manager_t *vm, uint16_t which)
{
  dt_view_t *v = vm->view + vm->current_view;
  if(v->key_pressed) v->key_pressed(v, which);
}

void dt_view_manager_configure (dt_view_manager_t *vm, int width, int height)
{
  for(int k=0;k<vm->num_views;k++)
  { // this is necessary for all
    dt_view_t *v = vm->view + k;
    if(v->configure) v->configure(v, width, height);
  }
}

void dt_view_manager_scrolled (dt_view_manager_t *vm, double x, double y, int up)
{
  dt_view_t *v = vm->view + vm->current_view;
  if(v->scrolled) v->scrolled(v, x, y, up);
}

