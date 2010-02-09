
#include "common/darktable.h"
#include "develop/develop.h"
#include "views/view.h"
#include "gui/gtk.h"
#include <stdlib.h>
#include <stdio.h>
#include <glib.h>
#include <string.h>
#include <strings.h>
#include <glade/glade.h>

void dt_view_manager_init(dt_view_manager_t *vm)
{
  vm->num_views = 0;
  // TODO: load all in directory?
  int k = -1;
  k = dt_view_manager_load_module(vm, "darkroom");
  // FIXME: this is global for plugins etc.
  if(k >= 0) darktable.develop = (dt_develop_t *)vm->view[k].data;
  vm->current_view = dt_view_manager_load_module(vm, "lighttable");
  vm->current_view = -1;
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
  int retval = -1;
  bzero(view, sizeof(dt_view_t));
  view->data = NULL;
  strncpy(view->module_name, module, 64);
  char plugindir[1024];
  dt_get_plugindir(plugindir, 1024);
  strcpy(plugindir + strlen(plugindir), "/views");
  gchar *libname = g_module_build_path(plugindir, (const gchar *)module);
  view->module = g_module_open(libname, G_MODULE_BIND_LAZY);
  if(!view->module)
  {
    fprintf(stderr, "[view_load_module] could not open %s (%s)!\n", libname, g_module_error());
    retval = -1;
    goto out;
  }
  int (*version)();
  if(!g_module_symbol(view->module, "dt_module_version", (gpointer)&(version))) goto out;
  if(version() != dt_version())
  {
    fprintf(stderr, "[view_load_module] `%s' is compiled for another version of dt (module %d != dt %d) !\n", libname, version(), dt_version());
    goto out;
  }
  if(!g_module_symbol(view->module, "name",            (gpointer)&(view->name)))            view->name = NULL;
  if(!g_module_symbol(view->module, "init",            (gpointer)&(view->init)))            view->init = NULL;
  if(!g_module_symbol(view->module, "cleanup",         (gpointer)&(view->cleanup)))         view->cleanup = NULL;
  if(!g_module_symbol(view->module, "expose",          (gpointer)&(view->expose)))          view->expose = NULL;
  if(!g_module_symbol(view->module, "try_enter",       (gpointer)&(view->try_enter)))       view->try_enter = NULL;
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
  if(!g_module_symbol(view->module, "border_scrolled", (gpointer)&(view->border_scrolled))) view->border_scrolled = NULL;

  if(view->init) view->init(view);

  /* success */
  retval = 0;

out:
  g_free(libname);
  return retval;
}

/** unload, cleanup */
void dt_view_unload_module (dt_view_t *view)
{
  if(view->cleanup) view->cleanup(view);
  if(view->module) g_module_close(view->module);
}

void dt_vm_remove_child(GtkWidget *widget, gpointer data)
{
  gtk_container_remove(GTK_CONTAINER(data), widget);
}

int dt_view_manager_switch (dt_view_manager_t *vm, int k)
{
  // destroy old module list
  GtkContainer *table = GTK_CONTAINER(glade_xml_get_widget (darktable.gui->main_window, "module_list"));
  gtk_container_foreach(table, (GtkCallback)dt_vm_remove_child, (gpointer)table);

  int error = 0;
  dt_view_t *v = vm->view + vm->current_view;
  int newv = vm->current_view;
  if(k < vm->num_views) newv = k;
  dt_view_t *nv = vm->view + newv;
  if(nv->try_enter) error = nv->try_enter(nv);
  if(!error)
  {
    if(vm->current_view >= 0 && v->leave) v->leave(v);
    vm->current_view = newv;
    if(newv >= 0 && nv->enter) nv->enter(nv);
  }
  return error;
}

const char *dt_view_manager_name (dt_view_manager_t *vm)
{
  if(vm->current_view < 0) return "";
  dt_view_t *v = vm->view + vm->current_view;
  if(v->name) return v->name(v);
  else return v->module_name;
}

void dt_view_manager_expose (dt_view_manager_t *vm, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  if(vm->current_view < 0)
  {
    cairo_set_source_rgb(cr, darktable.gui->bgcolor[0], darktable.gui->bgcolor[1], darktable.gui->bgcolor[2]);
    cairo_paint(cr);
    return;
  }
  dt_view_t *v = vm->view + vm->current_view;
  if(v->expose) v->expose(v, cr, width, height, pointerx, pointery);
}

void dt_view_manager_reset (dt_view_manager_t *vm)
{
  if(vm->current_view < 0) return;
  dt_view_t *v = vm->view + vm->current_view;
  if(v->reset) v->reset(v);
}

void dt_view_manager_mouse_leave (dt_view_manager_t *vm)
{
  if(vm->current_view < 0) return;
  dt_view_t *v = vm->view + vm->current_view;
  if(v->mouse_leave) v->mouse_leave(v);
}

void dt_view_manager_mouse_moved (dt_view_manager_t *vm, double x, double y, int which)
{
  if(vm->current_view < 0) return;
  dt_view_t *v = vm->view + vm->current_view;
  if(v->mouse_moved) v->mouse_moved(v, x, y, which);
}

void dt_view_manager_button_released (dt_view_manager_t *vm, double x, double y, int which, uint32_t state)
{
  if(vm->current_view < 0) return;
  dt_view_t *v = vm->view + vm->current_view;
  if(v->button_released) v->button_released(v, x, y, which, state);
}

void dt_view_manager_button_pressed (dt_view_manager_t *vm, double x, double y, int which, int type, uint32_t state)
{
  if(vm->current_view < 0) return;
  dt_view_t *v = vm->view + vm->current_view;
  if(v->button_pressed) v->button_pressed(v, x, y, which, type, state);
}

void dt_view_manager_key_pressed (dt_view_manager_t *vm, uint16_t which)
{
  if(vm->current_view < 0) return;
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
  if(vm->current_view < 0) return;
  dt_view_t *v = vm->view + vm->current_view;
  if(v->scrolled) v->scrolled(v, x, y, up);
}

void dt_view_manager_border_scrolled (dt_view_manager_t *vm, double x, double y, int which, int up)
{
  if(vm->current_view < 0) return;
  dt_view_t *v = vm->view + vm->current_view;
  if(v->border_scrolled) v->border_scrolled(v, x, y, which, up);
}

