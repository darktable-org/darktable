/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2011 henrik andersson

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

#include "common/collection.h"
#include "common/darktable.h"
#include "common/image_cache.h"
#include "common/debug.h"
#include "common/history.h"
#include "libs/lib.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "views/view.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include <stdlib.h>
#include <stdio.h>
#include <glib.h>
#include <string.h>
#include <strings.h>
#include <math.h>

void dt_view_manager_init(dt_view_manager_t *vm)
{
  /* prepare statements */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select * from selected_images where imgid = ?1", -1, &vm->statements.is_selected, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from selected_images where imgid = ?1", -1, &vm->statements.delete_from_selected, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into selected_images values (?1)", -1, &vm->statements.make_selected, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select num from history where imgid = ?1", -1, &vm->statements.have_history, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select color from color_labels where imgid=?1", -1, &vm->statements.get_color, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images where group_id = (select group_id from images where id=?1) and id != ?2", -1, &vm->statements.get_grouped, NULL);

  int res=0, midx=0;
  char *modules[] = {"lighttable","darkroom","capture",NULL};
  char *module = modules[midx];
  while(module != NULL)
  {
    if((res=dt_view_manager_load_module(vm, module))<0)
      fprintf(stderr,"[view_manager_init] failed to load view module '%s'\n",module);
    else
    {
      // Module loaded lets handle specific cases
      if(strcmp(module,"darkroom") == 0)
        darktable.develop = (dt_develop_t *)vm->view[res].data;
    }
    module = modules[++midx];
  }
  vm->current_view=-1;
}

void dt_view_manager_cleanup(dt_view_manager_t *vm)
{
  for(int k=0; k<vm->num_views; k++) dt_view_unload_module(vm->view + k);
}

const dt_view_t *dt_view_manager_get_current_view(dt_view_manager_t *vm)
{
  return &vm->view[vm->current_view];
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
  memset(view, 0, sizeof(dt_view_t));
  view->data = NULL;
  view->vscroll_size = view->vscroll_viewport_size = 1.0;
  view->hscroll_size = view->hscroll_viewport_size = 1.0;
  view->vscroll_pos = view->hscroll_pos = 0.0;
  view->height = view->width = 100; // set to non-insane defaults before first expose/configure.
  g_strlcpy(view->module_name, module, 64);
  char plugindir[1024];
  dt_util_get_plugindir(plugindir, 1024);
  g_strlcat(plugindir, "/views", 1024);
  gchar *libname = g_module_build_path(plugindir, (const gchar *)module);
  view->module = g_module_open(libname, G_MODULE_BIND_LAZY);
  if(!view->module)
  {
    fprintf(stderr, "[view_load_module] could not open %s (%s)!\n", libname, g_module_error());
    retval = -1;
    goto out;
  }
  int (*version)();
  if(!g_module_symbol(view->module, "dt_module_dt_version", (gpointer)&(version))) goto out;
  if(version() != dt_version())
  {
    fprintf(stderr, "[view_load_module] `%s' is compiled for another version of dt (module %d != dt %d) !\n", libname, version(), dt_version());
    goto out;
  }
  if(!g_module_symbol(view->module, "name",            (gpointer)&(view->name)))            view->name = NULL;
  if(!g_module_symbol(view->module, "view",            (gpointer)&(view->view)))            view->view = NULL;
  if(!g_module_symbol(view->module, "init",            (gpointer)&(view->init)))            view->init = NULL;
  if(!g_module_symbol(view->module, "cleanup",         (gpointer)&(view->cleanup)))         view->cleanup = NULL;
  if(!g_module_symbol(view->module, "expose",          (gpointer)&(view->expose)))          view->expose = NULL;
  if(!g_module_symbol(view->module, "try_enter",       (gpointer)&(view->try_enter)))       view->try_enter = NULL;
  if(!g_module_symbol(view->module, "enter",           (gpointer)&(view->enter)))           view->enter = NULL;
  if(!g_module_symbol(view->module, "leave",           (gpointer)&(view->leave)))           view->leave = NULL;
  if(!g_module_symbol(view->module, "reset",           (gpointer)&(view->reset)))           view->reset = NULL;
  if(!g_module_symbol(view->module, "mouse_enter",     (gpointer)&(view->mouse_enter)))     view->mouse_enter= NULL;
  if(!g_module_symbol(view->module, "mouse_leave",     (gpointer)&(view->mouse_leave)))     view->mouse_leave = NULL;
  if(!g_module_symbol(view->module, "mouse_moved",     (gpointer)&(view->mouse_moved)))     view->mouse_moved = NULL;
  if(!g_module_symbol(view->module, "button_released", (gpointer)&(view->button_released))) view->button_released = NULL;
  if(!g_module_symbol(view->module, "button_pressed",  (gpointer)&(view->button_pressed)))  view->button_pressed = NULL;
  if(!g_module_symbol(view->module, "key_pressed",     (gpointer)&(view->key_pressed)))     view->key_pressed = NULL;
  if(!g_module_symbol(view->module, "key_released",    (gpointer)&(view->key_released)))    view->key_released = NULL;
  if(!g_module_symbol(view->module, "configure",       (gpointer)&(view->configure)))       view->configure = NULL;
  if(!g_module_symbol(view->module, "scrolled",        (gpointer)&(view->scrolled)))        view->scrolled = NULL;
  if(!g_module_symbol(view->module, "border_scrolled", (gpointer)&(view->border_scrolled))) view->border_scrolled = NULL;
  if(!g_module_symbol(view->module, "init_key_accels", (gpointer)&(view->init_key_accels))) view->init_key_accels = NULL;
  if(!g_module_symbol(view->module, "connect_key_accels", (gpointer)&(view->connect_key_accels))) view->connect_key_accels = NULL;

  view->accel_closures = NULL;

  if(view->init) view->init(view);
  if(view->init_key_accels) view->init_key_accels(view);

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
  // Before switching views, restore accelerators if disabled
  if(!darktable.control->key_accelerators_on)
    dt_control_key_accelerators_on(darktable.control);

  // destroy old module list
  int error = 0;
  dt_view_t *v = vm->view + vm->current_view;

  /* Special case when entering nothing (just before leaving dt) */
  if ( k==DT_MODE_NONE )
  {
    /* iterator plugins and cleanup plugins in current view */
    GList *plugins = g_list_last(darktable.lib->plugins);
    while (plugins)
    {
      dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

      if (!plugin && !plugin->views)
        fprintf(stderr,"module %s doesnt have views flags\n",plugin->name());

      /* does this module belong to current view ?*/
      if (plugin->views() & v->view(v) )
      {
        plugin->gui_cleanup(plugin);
        dt_accel_disconnect_list(plugin->accel_closures);
        plugin->accel_closures = NULL;
      }

      /* get next plugin */
      plugins = g_list_previous(plugins);
    }

    /* leave the current view*/
    if(vm->current_view >= 0 && v->leave) v->leave(v);

    /* remove all widets in all containers */
    for(int l=0;l<DT_UI_CONTAINER_SIZE;l++)
      dt_ui_container_clear(darktable.gui->ui, l);

    vm->current_view = -1 ;
    return 0 ;
  }

  int newv = vm->current_view;
  if (k < vm->num_views) newv = k;
  dt_view_t *nv = vm->view + newv;

  if (nv->try_enter) 
    error = nv->try_enter(nv);

  if (!error)
  {
    GList *plugins;
    
    /* cleanup current view before initialization of new  */
    if (vm->current_view >=0)
    {
      /* leave current view */
      if(v->leave) v->leave(v);
      dt_accel_disconnect_list(v->accel_closures);
      v->accel_closures = NULL;

      /* iterator plugins and cleanup plugins in current view */
      plugins = g_list_last(darktable.lib->plugins);
      while (plugins)
      {
        dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

        if (!plugin && !plugin->views)
          fprintf(stderr,"module %s doesnt have views flags\n",plugin->name());

        /* does this module belong to current view ?*/
        if (plugin->views() & v->view(v) )
        {
          plugin->gui_cleanup(plugin);
          dt_accel_disconnect_list(plugin->accel_closures);
          plugin->accel_closures = NULL;
        }

        /* get next plugin */
        plugins = g_list_previous(plugins);
      }

      /* remove all widets in all containers */
      for(int l=0;l<DT_UI_CONTAINER_SIZE;l++)
        dt_ui_container_clear(darktable.gui->ui, l);
    }

    /* change current view to the new view */
    vm->current_view = newv;

    /* restore visible stat of panels for the new view */
    dt_ui_restore_panels(darktable.gui->ui);

    /* lets add plugins related to new view into panels */
    plugins = g_list_last(darktable.lib->plugins);
    while (plugins)
    {
      dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);
      if( plugin->views() & nv->view(v) )
      {
        /* module should be in this view, lets initialize */
        plugin->gui_init(plugin);

        /* try get the module expander  */
        GtkWidget *w = NULL;
        w = dt_lib_gui_get_expander(plugin);

        if(plugin->connect_key_accels)
          plugin->connect_key_accels(plugin);
        dt_lib_connect_common_accels(plugin);

        /* if we dont got an expander lets add the widget */
        if (!w)
          w = plugin->widget;

        /* add module to it's container */
        dt_ui_container_add_widget(darktable.gui->ui, plugin->container(), w);

      }

      /* lets get next plugin */
      plugins = g_list_previous(plugins);
    }

    /* hide/show modules as last config */
    plugins = g_list_last(darktable.lib->plugins);
    while (plugins)
    {
      dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);
      if(plugin->views() & nv->view(v))
      {
        /* set expanded if last mode was that */
        char var[1024];
        gboolean expanded = FALSE;
        if (plugin->expandable())
        {
          snprintf(var, 1024, "plugins/lighttable/%s/expanded", plugin->plugin_name);
          expanded = dt_conf_get_bool(var);
	  
	  dt_lib_gui_set_expanded(plugin, expanded);
	
          /* show expander if visible  */
          if(dt_lib_is_visible(plugin))
            gtk_widget_show_all(GTK_WIDGET(plugin->expander));
          else
            gtk_widget_hide(GTK_WIDGET(plugin->expander));
        }

        /* show/hide plugin widget depending on expanded flag or if plugin
	   not is expandeable() */
        if(dt_lib_is_visible(plugin) && (expanded || !plugin->expandable()))
          gtk_widget_show_all(plugin->widget);
        else
          gtk_widget_hide_all(plugin->widget);
      }

      /* lets get next plugin */
      plugins = g_list_previous(plugins); 
    }


    /* enter view */
    if(newv >= 0 && nv->enter) nv->enter(nv);
    if(newv >= 0 && nv->connect_key_accels)
      nv->connect_key_accels(nv);


    /* raise view changed signal */
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED);

    /* add endmarkers to left and right center containers */
    GtkWidget *endmarker = gtk_drawing_area_new();
    dt_ui_container_add_widget(darktable.gui->ui, DT_UI_CONTAINER_PANEL_LEFT_CENTER, endmarker);
    g_signal_connect (G_OBJECT (endmarker), "expose-event",
                      G_CALLBACK (dt_control_expose_endmarker), 0);
    gtk_widget_set_size_request(endmarker, -1, 50);
    gtk_widget_show(endmarker);

    endmarker = gtk_drawing_area_new();
    dt_ui_container_add_widget(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER, endmarker);
    g_signal_connect (G_OBJECT (endmarker), "expose-event",
                      G_CALLBACK (dt_control_expose_endmarker), (gpointer)1);
    gtk_widget_set_size_request(endmarker, -1, 50);
    gtk_widget_show(endmarker);
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
  v->width = width;
  v->height = height;

  if(v->expose)
  {
    /* expose the view */
    cairo_rectangle(cr, 0, 0, v->width, v->height);
    cairo_clip(cr);
    cairo_new_path(cr);
    float px = pointerx, py = pointery;
    if(pointery > v->height)
    {
      px = 10000.0;
      py = -1.0;
    }
    v->expose(v, cr, v->width, v->height, px, py);
  
    /* expose plugins */
    GList *plugins = g_list_last(darktable.lib->plugins);
    while (plugins)
    {
      dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);
      
      if (!plugin && !plugin->views)
        fprintf(stderr,"module %s doesnt have views flags\n",plugin->name());
	  
      /* does this module belong to current view ?*/
      if (plugin->gui_post_expose && plugin->views() & v->view(v) )
	plugin->gui_post_expose(plugin,cr,v->width,v->height,px,py );

      /* get next plugin */
      plugins = g_list_previous(plugins);
    } 
  }
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

void dt_view_manager_mouse_enter (dt_view_manager_t *vm)
{
  if(vm->current_view < 0) return;
  dt_view_t *v = vm->view + vm->current_view;
  if(v->mouse_enter) v->mouse_enter(v);
}

void dt_view_manager_mouse_moved (dt_view_manager_t *vm, double x, double y, int which)
{
  if(vm->current_view < 0) return;
  dt_view_t *v = vm->view + vm->current_view;

  /* lets check if any plugins want to handle mouse move */
  gboolean handled = FALSE;
  GList *plugins = g_list_last(darktable.lib->plugins);
  while (plugins)
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);
    
    /* does this module belong to current view ?*/
    if (plugin->mouse_moved && plugin->views() & v->view(v) )
      if(plugin->mouse_moved(plugin, x, y, which))
	handled = TRUE;
    
    /* get next plugin */
    plugins = g_list_previous(plugins);
  } 
  
  /* if not handled by any plugin let pass to view handler*/
  if(!handled && v->mouse_moved) 
    v->mouse_moved(v, x, y, which);

}

int dt_view_manager_button_released (dt_view_manager_t *vm, double x, double y, int which, uint32_t state)
{
  if(vm->current_view < 0) return 0;
  dt_view_t *v = vm->view + vm->current_view;

  /* lets check if any plugins want to handle button press */
  gboolean handled = FALSE;
  GList *plugins = g_list_last(darktable.lib->plugins);
  while (plugins)
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);
    
    /* does this module belong to current view ?*/
    if (plugin->button_released && plugin->views() & v->view(v) )
      if(plugin->button_released(plugin, x, y, which,state))
	handled = TRUE;
    
    /* get next plugin */
    plugins = g_list_previous(plugins);
  } 
  
  /* if not handled by any plugin let pass to view handler*/
  if(!handled && v->button_released) 
    v->button_released(v, x, y, which,state);

  return 0;
}

int dt_view_manager_button_pressed (dt_view_manager_t *vm, double x, double y, int which, int type, uint32_t state)
{
  if(vm->current_view < 0) return 0;
  dt_view_t *v = vm->view + vm->current_view;

  /* lets check if any plugins want to handle button press */
  gboolean handled = FALSE;
  GList *plugins = g_list_last(darktable.lib->plugins);
  while (plugins)
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);
    
    /* does this module belong to current view ?*/
    if (plugin->button_pressed && plugin->views() & v->view(v) )
      if(plugin->button_pressed(plugin, x, y, which,type,state))
	handled = TRUE;
    
    /* get next plugin */
    plugins = g_list_previous(plugins);
  } 

  /* if not handled by any plugin let pass to view handler*/
  if(!handled && v->button_pressed) 
    v->button_pressed(v, x, y, which,type,state);

  return 0;
}

int dt_view_manager_key_pressed (dt_view_manager_t *vm, guint key, guint state)
{
  int film_strip_result = 0;
  if(vm->current_view < 0) return 0;
  dt_view_t *v = vm->view + vm->current_view;
  if(v->key_pressed)
    return v->key_pressed(v, key, state) || film_strip_result;
  return 0;
}

int dt_view_manager_key_released (dt_view_manager_t *vm, guint key, guint state)
{
  int film_strip_result = 0;
  if(vm->current_view < 0) return 0;
  dt_view_t *v = vm->view + vm->current_view;

  if(v->key_released)
    return v->key_released(v, key, state) || film_strip_result;

  return 0;
}

void dt_view_manager_configure (dt_view_manager_t *vm, int width, int height)
{
  for(int k=0; k<vm->num_views; k++)
  {
    // this is necessary for all
    dt_view_t *v = vm->view + k;
    v->width = width;
    v->height = height;
    if(v->configure) v->configure(v, width, height);
  }
}

void dt_view_manager_scrolled (dt_view_manager_t *vm, double x, double y, int up, int state)
{
  if(vm->current_view < 0) return;
  dt_view_t *v = vm->view + vm->current_view;
  if(v->scrolled) 
    v->scrolled(v, x, y, up, state);
}

void dt_view_manager_border_scrolled (dt_view_manager_t *vm, double x, double y, int which, int up)
{
  if(vm->current_view < 0) return;
  dt_view_t *v = vm->view + vm->current_view;
  if(v->border_scrolled) v->border_scrolled(v, x, y, which, up);
}

void dt_view_set_scrollbar(dt_view_t *view, float hpos, float hsize, float hwinsize, float vpos, float vsize, float vwinsize)
{
  view->vscroll_pos = vpos;
  view->vscroll_size = vsize;
  view->vscroll_viewport_size = vwinsize;
  view->hscroll_pos = hpos;
  view->hscroll_size = hsize;
  view->hscroll_viewport_size = hwinsize;
  GtkWidget *widget;
  widget = darktable.gui->widgets.left_border;
  gtk_widget_queue_draw(widget);
  widget = darktable.gui->widgets.right_border;
  gtk_widget_queue_draw(widget);
  widget = darktable.gui->widgets.bottom_border;
  gtk_widget_queue_draw(widget);
  widget = darktable.gui->widgets.top_border;
  gtk_widget_queue_draw(widget);
}

static inline void
dt_view_draw_altered(cairo_t *cr, const float x, const float y, const float r)
{
  cairo_new_sub_path(cr);
  cairo_arc(cr, x, y, r, 0, 2.0f*M_PI);
  const float dx = r*cosf(M_PI/8.0f), dy = r*sinf(M_PI/8.0f);
  cairo_move_to(cr, x-dx, y-dy);
  cairo_curve_to(cr, x, y-2*dy, x, y+2*dy, x+dx, y+dy);
  cairo_move_to(cr, x-.20*dx, y+.8*dy);
  cairo_line_to(cr, x-.80*dx, y+.8*dy);
  cairo_move_to(cr, x+.20*dx, y-.8*dy);
  cairo_line_to(cr, x+.80*dx, y-.8*dy);
  cairo_move_to(cr, x+.50*dx, y-.8*dy-0.3*dx);
  cairo_line_to(cr, x+.50*dx, y-.8*dy+0.3*dx);
  cairo_stroke(cr);
}

static inline void
dt_view_star(cairo_t *cr, float x, float y, float r1, float r2)
{
  const float d = 2.0*M_PI*0.1f;
  const float dx[10] = {sinf(0.0), sinf(d), sinf(2*d), sinf(3*d), sinf(4*d), sinf(5*d), sinf(6*d), sinf(7*d), sinf(8*d), sinf(9*d)};
  const float dy[10] = {cosf(0.0), cosf(d), cosf(2*d), cosf(3*d), cosf(4*d), cosf(5*d), cosf(6*d), cosf(7*d), cosf(8*d), cosf(9*d)};
  cairo_move_to(cr, x+r1*dx[0], y-r1*dy[0]);
  for(int k=1; k<10; k++)
    if(k&1) cairo_line_to(cr, x+r2*dx[k], y-r2*dy[k]);
    else    cairo_line_to(cr, x+r1*dx[k], y-r1*dy[k]);
  cairo_close_path(cr);
}


void dt_view_image_expose(dt_image_t *img, dt_view_image_over_t *image_over, int32_t index, cairo_t *cr, int32_t width, int32_t height, int32_t zoom, int32_t px, int32_t py)
{
  cairo_save (cr);
  const int32_t imgid = img ? img->id : index; // in case of locked image, use id to draw basic stuff.
  float bgcol = 0.4, fontcol = 0.5, bordercol = 0.1, outlinecol = 0.2;
  int selected = 0, altered = 0, imgsel, is_grouped = 0;
  DT_CTL_GET_GLOBAL(imgsel, lib_image_mouse_over_id);
  // if(img->flags & DT_IMAGE_SELECTED) selected = 1;

  /* clear and reset statements */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.is_selected);
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.have_history);
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.get_grouped);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.is_selected);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.have_history);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.get_grouped);

  /* bind imgid to prepared statments */
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.is_selected, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.have_history, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.get_grouped, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.get_grouped, 2, imgid);

  /* lets check if imgid is selected */
  if(sqlite3_step(darktable.view_manager->statements.is_selected) == SQLITE_ROW) 
    selected = 1;

  /* lets check if imgid has history */
  if(sqlite3_step(darktable.view_manager->statements.have_history) == SQLITE_ROW) 
    altered = 1;

  /* lets check if imgid is in a group */
  if(sqlite3_step(darktable.view_manager->statements.get_grouped) == SQLITE_ROW)
    is_grouped = 1;
  else if(img && darktable.gui->expanded_group_id == img->group_id)
    darktable.gui->expanded_group_id = -1;

  if(selected == 1)
  {
    outlinecol = 0.4;
    bgcol = 0.6;
    fontcol = 0.5;
  }
  if(imgsel == imgid)
  {
    bgcol = 0.8;  // mouse over
    fontcol = 0.7;
    outlinecol = 0.6;
  }
  float imgwd = 0.8f;
  if(zoom == 1)
  {
    imgwd = .97f;
    // cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  }
  else
  {
    double x0 = 0.007*width, y0 = 0.007*height, rect_width = 0.986*width, rect_height = 0.986*height, radius = 0.04*width;
    // double x0 = 0.*width, y0 = 0.*height, rect_width = 1.*width, rect_height = 1.*height, radius = 0.08*width;
    double x1, y1, off, off1;

    x1=x0+rect_width;
    y1=y0+rect_height;
    off=radius*0.666;
    off1 = radius-off;
    cairo_move_to  (cr, x0, y0 + radius);
    cairo_curve_to (cr, x0, y0+off1, x0+off1 , y0, x0 + radius, y0);
    cairo_line_to (cr, x1 - radius, y0);
    cairo_curve_to (cr, x1-off1, y0, x1, y0+off1, x1, y0 + radius);
    cairo_line_to (cr, x1 , y1 - radius);
    cairo_curve_to (cr, x1, y1-off1, x1-off1, y1, x1 - radius, y1);
    cairo_line_to (cr, x0 + radius, y1);
    cairo_curve_to (cr, x0+off1, y1, x0, y1-off1, x0, y1- radius);
    cairo_close_path (cr);
    cairo_set_source_rgb(cr, bgcol, bgcol, bgcol);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 0.005*width);
    cairo_set_source_rgb(cr, outlinecol, outlinecol, outlinecol);
    cairo_stroke(cr);

    if(img)
    {
      const char *ext = img->filename + strlen(img->filename);
      while(ext > img->filename && *ext != '.') ext--;
      ext++;
      cairo_set_source_rgb(cr, fontcol, fontcol, fontcol);
      cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size (cr, .25*width);

      cairo_move_to (cr, .01*width, .24*height);
      cairo_show_text (cr, ext);
    }
  }

#if 1
  int32_t iwd = width*imgwd, iht = height*imgwd, stride;
  float fwd=0, fht=0;
  float scale = 1.0;
  dt_image_buffer_t mip = DT_IMAGE_NONE;
  if(img)
  {
    mip = dt_image_get_matching_mip_size(img, imgwd*width, imgwd*height, &iwd, &iht);
    mip = dt_image_get(img, mip, 'r');
    iwd = img->mip_width[mip];
    iht = img->mip_height[mip];
    fwd = img->mip_width_f[mip]-3;
    fht = img->mip_height_f[mip]-3;
  }
  cairo_surface_t *surface = NULL;
  if(mip != DT_IMAGE_NONE)
  {
    stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, iwd);
    surface = cairo_image_surface_create_for_data (img->mip[mip], CAIRO_FORMAT_RGB24, iwd, iht, stride);
    scale = fminf(width*imgwd/fwd, height*imgwd/fht);
  }

  // draw centered and fitted:
  cairo_save(cr);
  cairo_translate(cr, width/2.0, height/2.0f);
  cairo_scale(cr, scale, scale);
  cairo_translate(cr, -.5f*(fwd), -.5f*(fht));

  if(mip != DT_IMAGE_NONE)
  {
    cairo_set_source_surface (cr, surface, -1, -1);
    cairo_rectangle(cr, 0, 0, fwd, fht);
    cairo_fill(cr);
    cairo_surface_destroy (surface);
    dt_image_release(img, mip, 'r');

    if(zoom == 1) cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_rectangle(cr, 0, 0, fwd, fht);
  }

  // border around image
  const float border = zoom == 1 ? 16/scale : 2/scale;
  cairo_set_source_rgb(cr, bordercol, bordercol, bordercol);
  if(mip != DT_IMAGE_NONE && selected)
  {
    cairo_set_line_width(cr, 1./scale);
    if(zoom == 1)
    {
      // draw shadow around border
      cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
      cairo_stroke(cr);
      // cairo_new_path(cr);
      cairo_set_fill_rule (cr, CAIRO_FILL_RULE_EVEN_ODD);
      float alpha = 1.0f;
      for(int k=0; k<16; k++)
      {
        cairo_rectangle(cr, 0, 0, fwd, fht);
        cairo_new_sub_path(cr);
        cairo_rectangle(cr, -k/scale, -k/scale, fwd+2.*k/scale, fht+2.*k/scale);
        cairo_set_source_rgba(cr, 0, 0, 0, alpha);
        alpha *= 0.6f;
        cairo_fill(cr);
      }
    }
    else
    {
      cairo_set_fill_rule (cr, CAIRO_FILL_RULE_EVEN_ODD);
      cairo_new_sub_path(cr);
      cairo_rectangle(cr, -border, -border, fwd+2.*border, fht+2.*border);
      cairo_stroke_preserve(cr);
      cairo_set_source_rgb(cr, 1.0-bordercol, 1.0-bordercol, 1.0-bordercol);
      cairo_fill(cr);
    }
  }
  else if(mip != DT_IMAGE_NONE)
  {
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
  }
  cairo_restore(cr);

  const float fscale = fminf(width, height);
  if(imgsel == imgid)
  {
    // draw mouseover hover effects, set event hook for mouse button down!
    *image_over = DT_VIEW_DESERT;
    cairo_set_line_width(cr, 1.5);
    cairo_set_source_rgb(cr, outlinecol, outlinecol, outlinecol);
    cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
    float r1, r2;
    if(zoom != 1)
    {
      r1 = 0.05*width;
      r2 = 0.022*width;
    }
    else
    {
      r1 = 0.015*fscale;
      r2 = 0.007*fscale;
    }

    float x, y;
    if(zoom != 1) y = 0.90*height;
    else y = .12*fscale;

    if(img) for(int k=0; k<5; k++)
      {
        if(zoom != 1) x = (0.41+k*0.12)*width;
        else x = (.08+k*0.04)*fscale;

        if((img->flags & 0x7) != 6) //if rejected: draw no stars
        {
          dt_view_star(cr, x, y, r1, r2);
          if((px - x)*(px - x) + (py - y)*(py - y) < r1*r1)
          {
            *image_over = DT_VIEW_STAR_1 + k;
            cairo_fill(cr);
          }
          else if(img && ((img->flags & 0x7) > k))
          {
            cairo_fill_preserve(cr);
            cairo_set_source_rgb(cr, 1.0-bordercol, 1.0-bordercol, 1.0-bordercol);
            cairo_stroke(cr);
            cairo_set_source_rgb(cr, outlinecol, outlinecol, outlinecol);
          }
          else cairo_stroke(cr);
        }
      }

    //Image rejected?
    if(zoom !=1) x = 0.11*width;
    else x = .04*fscale;

    if((px - x)*(px - x) + (py - y)*(py - y) < r1*r1)
    {
      *image_over = DT_VIEW_REJECT; //mouse sensitive
      cairo_new_sub_path(cr);
      cairo_arc(cr, x, y, (r1+r2)*.5, 0, 2.0f*M_PI);
      cairo_stroke(cr);
    }
    else if (img && ((img->flags & 0x7) == 6))
    {
      cairo_set_source_rgb(cr, 1., 0., 0.);
      cairo_new_sub_path(cr);
      cairo_arc(cr, x, y, (r1+r2)*.5, 0, 2.0f*M_PI);
      cairo_stroke(cr);
      cairo_set_line_width(cr, 2.5);
    }

    //reject cross:
    cairo_move_to(cr, x-r2, y-r2);
    cairo_line_to(cr, x+r2, y+r2);
    cairo_move_to(cr, x+r2, y-r2);
    cairo_line_to(cr, x-r2, y+r2);
    cairo_close_path(cr);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, outlinecol, outlinecol, outlinecol);
    cairo_set_line_width(cr, 1.5);

    // image part of a group?
    if(is_grouped && darktable.gui && darktable.gui->grouping)
    {
      // draw grouping icon and border if the current group is expanded
      // align to the right, left of altered
      float s = (r1+r2)*.75;
      float _x, _y;
      if(zoom != 1)
      {
        _x = width*0.9 - s*2.5;
        _y = height*0.1 - s*.4;
      }
      else
      {
        _x = (.04+7*0.04-1.1*.04)*fscale;
        _y = y - (.17*.04)*fscale;
      }
      cairo_save(cr);
      if(imgid != img->group_id)
        cairo_set_source_rgb(cr, fontcol, fontcol, fontcol);
      dtgtk_cairo_paint_grouping(cr, _x, _y, s, s, 23);
      cairo_restore(cr);
      // mouse is over the grouping icon
      if(img && abs(px-_x-.5*s) <= .8*s && abs(py-_y-.5*s) <= .8*s)
        *image_over = DT_VIEW_GROUP;
    }

    // image altered?
    if(altered)
    {
      // align to right
      float s = (r1+r2)*.5;
      if(zoom != 1)
      {
        x = width*0.9;
        y = height*0.1;
      }
      else x = (.04+7*0.04)*fscale;
      dt_view_draw_altered(cr, x, y, s);
      //g_print("px = %d, x = %.4f, py = %d, y = %.4f\n", px, x, py, y);
      if(img && abs(px-x) <= 1.2*s && abs(py-y) <= 1.2*s) // mouse hovers over the altered-icon -> history tooltip!
      {
        if(darktable.gui->center_tooltip == 0) // no tooltip yet, so add one
        {
          char* tooltip = dt_history_get_items_as_string(img->id);
          if(tooltip != NULL)
          {
            g_object_set(G_OBJECT(dt_ui_center(darktable.gui->ui)), "tooltip-text", tooltip, (char *)NULL);
            g_free(tooltip);
          }
        }
        darktable.gui->center_tooltip = 1;
      }
    }
  }

  // kill all paths, in case img was not loaded yet, or is blocked:
  cairo_new_path(cr);

  // TODO: make mouse sensitive, just as stars!
  {
    // color labels:
    const float x = zoom == 1 ? (0.07)*fscale : .21*width;
    const float y = zoom == 1 ? 0.17*fscale: 0.1*height;
    const float r = zoom == 1 ? 0.01*fscale : 0.03*width;

    /* clear and reset prepared statement */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.get_color);
    DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.get_color); 

    /* setup statement and iterate rows */
    DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.get_color, 1, imgid);
    while(sqlite3_step(darktable.view_manager->statements.get_color) == SQLITE_ROW)
    {
      cairo_save(cr);
      int col = sqlite3_column_int(darktable.view_manager->statements.get_color, 0);
      // see src/dtgtk/paint.c
      dtgtk_cairo_paint_label(cr, x+(3*r*col)-5*r, y-r, r*2, r*2, col);
      cairo_restore(cr);
    }
  }

  if(img && (zoom == 1))
  {
    // some exif data
    cairo_set_source_rgb(cr, .7, .7, .7);
    cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size (cr, .025*fscale);

    cairo_move_to (cr, .02*fscale, .04*fscale);
    // cairo_show_text(cr, img->filename);
    cairo_text_path(cr, img->filename);
    char exifline[50];
    cairo_move_to (cr, .02*fscale, .08*fscale);
    dt_image_print_exif(img, exifline, 50);
    cairo_text_path(cr, exifline);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_stroke(cr);
  }

  cairo_restore(cr);
  // if(zoom == 1) cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
#endif
}


/**
 * \brief Set the selection bit to a given value for the specified image
 * \param[in] imgid The image id
 * \param[in] value The boolean value for the bit
 */
void dt_view_set_selection(int imgid, int value)
{
  /* clear and reset statement */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.is_selected);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.is_selected);

  /* setup statement and iterate over rows */
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.is_selected, 1, imgid);

  if(sqlite3_step(darktable.view_manager->statements.is_selected) == SQLITE_ROW)
  {
    if(!value)
    {
      /* Value is set and should be unset; get rid of it */

      /* clear and reset statement */
      DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.delete_from_selected);
      DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.delete_from_selected);

      /* setup statement and execute */
      DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.delete_from_selected, 1, imgid);
      sqlite3_step(darktable.view_manager->statements.delete_from_selected);
    }
  }
  else if(value)
  {
    /* Select bit is unset and should be set; add it */
    
    /* clear and reset statement */ 
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.make_selected);
    DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.make_selected);
    
    /* setup statement and execute */  
    DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.make_selected, 1, imgid);
    sqlite3_step(darktable.view_manager->statements.make_selected);
  }
 
}

/**
 * \brief Toggle the selection bit in the database for the specified image
 * \param[in] imgid The image id
 */
void dt_view_toggle_selection(int imgid)
{

  /* clear and reset statement */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.is_selected);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.is_selected);

  /* setup statement and iterate over rows */
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.is_selected, 1, imgid);
  if(sqlite3_step(darktable.view_manager->statements.is_selected) == SQLITE_ROW)
  {
    /* clear and reset statement */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.delete_from_selected);
    DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.delete_from_selected);

    /* setup statement and execute */
    DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.delete_from_selected, 1, imgid);
    sqlite3_step(darktable.view_manager->statements.delete_from_selected);
  }
  else
  {
    /* clear and reset statement */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.make_selected);
    DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.make_selected);

    /* setup statement and execute */
    DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.make_selected, 1, imgid);
    sqlite3_step(darktable.view_manager->statements.make_selected);
  }
}

void dt_view_filmstrip_scroll_to_image(dt_view_manager_t *vm, const int imgid)
{
  //g_return_if_fail(vm->proxy.filmstrip.module!=NULL); // This can happend here for debugging
  //g_return_if_fail(vm->proxy.filmstrip.scroll_to_image!=NULL);

  if(vm->proxy.filmstrip.module && vm->proxy.filmstrip.scroll_to_image)
    vm->proxy.filmstrip.scroll_to_image(vm->proxy.filmstrip.module, imgid);
}

int32_t dt_view_filmstrip_get_activated_imgid(dt_view_manager_t *vm)
{
  //g_return_val_if_fail(vm->proxy.filmstrip.module!=NULL, 0); // This can happend here for debugging
  //g_return_val_if_fail(vm->proxy.filmstrip.activated_image!=NULL, 0);

  if(vm->proxy.filmstrip.module && vm->proxy.filmstrip.activated_image)
    return vm->proxy.filmstrip.activated_image(vm->proxy.filmstrip.module);

  return 0;
}

void dt_view_filmstrip_set_active_image(dt_view_manager_t *vm,int iid)
{
  /* First off clear all selected images... */
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "delete from selected_images", NULL, NULL, NULL);

  /* clear and reset statement */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.make_selected);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.make_selected);

  /* setup statement and execute */
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.make_selected, 1, iid);
  sqlite3_step(darktable.view_manager->statements.make_selected);

  dt_view_filmstrip_scroll_to_image(vm,iid);
}

void dt_view_filmstrip_prefetch()
{
  char query[1024];
  const gchar *qin = dt_collection_get_query (darktable.collection);
  int offset = 0;
  if(qin)
  {
    int imgid = -1;
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select imgid from selected_images", -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
      imgid = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    snprintf(query, 1024, "select rowid from (%s) where id=?3", qin);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1,  0);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
      offset = sqlite3_column_int(stmt, 0) - 1;
    sqlite3_finalize(stmt);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), qin, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, offset+1);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, 2);
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      imgid = sqlite3_column_int(stmt, 0);
      dt_image_t *image = dt_image_cache_get(imgid, 'r');
      dt_image_prefetch(image, DT_IMAGE_MIPF);
      dt_image_cache_release(image, 'r');
    }
    sqlite3_finalize(stmt);
  }
}

void dt_view_manager_view_toolbox_add(dt_view_manager_t *vm,GtkWidget *tool)
{
  if (vm->proxy.view_toolbox.module)
    vm->proxy.view_toolbox.add(vm->proxy.view_toolbox.module,tool);
}

void dt_view_lighttable_set_zoom(dt_view_manager_t *vm, gint zoom)
{
  if (vm->proxy.lighttable.module)
    vm->proxy.lighttable.set_zoom(vm->proxy.lighttable.module, zoom);
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
