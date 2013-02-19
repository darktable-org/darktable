/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2011-2012 henrik andersson.
    copyright (c) 2012 tobias ellinghaus.

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

#include "common/darktable.h"
#include "common/collection.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/debug.h"
#include "common/history.h"
#include "libs/lib.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "views/view.h"
#include "views/undo.h"
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
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert or ignore into selected_images values (?1)", -1, &vm->statements.make_selected, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select num from history where imgid = ?1", -1, &vm->statements.have_history, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select color from color_labels where imgid=?1", -1, &vm->statements.get_color, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images where group_id = (select group_id from images where id=?1) and id != ?2", -1, &vm->statements.get_grouped, NULL);

  int res=0, midx=0;
  char *modules[] =
  {
    "lighttable",
    "darkroom",
#ifdef HAVE_GPHOTO2
    "capture",
#endif
#ifdef HAVE_MAP
    "map",
#endif
    NULL
  };
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
  dt_loc_get_plugindir(plugindir, 1024);
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

  /*  clear the undo list, for now we do this inconditionally. At some point we will probably want to clear only part
      of the undo list. This should probably done with a view proxy routine returning the type of undo to remove. */
  dt_undo_clear(darktable.undo, DT_UNDO_ALL);

  /* Special case when entering nothing (just before leaving dt) */
  if ( k==DT_MODE_NONE )
  {
    /* leave the current view*/
    if(vm->current_view >= 0 && v->leave) v->leave(v);

    /* iterator plugins and cleanup plugins in current view */
    GList *plugins = g_list_last(darktable.lib->plugins);
    while (plugins)
    {
      dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

      if (!plugin->views)
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
    for(int l=0; l<DT_UI_CONTAINER_SIZE; l++)
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

        if (!plugin->views)
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
      for(int l=0; l<DT_UI_CONTAINER_SIZE; l++)
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
      if( plugin->views() & nv->view(nv) )
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
      if(plugin->views() & nv->view(nv))
      {
        /* set expanded if last mode was that */
        char var[1024];
        gboolean expanded = FALSE;
        gboolean visible = dt_lib_is_visible(plugin);
        if (plugin->expandable())
        {
          snprintf(var, 1024, "plugins/lighttable/%s/expanded", plugin->plugin_name);
          expanded = dt_conf_get_bool(var);

          /* show expander if visible  */
          if(visible)
          {
            gtk_widget_show_all(GTK_WIDGET(plugin->expander));
            // gtk_widget_show_all(plugin->widget);
          }
          else
          {
            gtk_widget_hide(GTK_WIDGET(plugin->expander));
            // gtk_widget_hide_all(plugin->widget);
          }

          dt_lib_gui_set_expanded(plugin, expanded);
        }
        else
        {
          /* show/hide plugin widget depending on expanded flag or if plugin
             not is expandeable() */
          if(visible)
            gtk_widget_show_all(plugin->widget);
          else
            gtk_widget_hide_all(plugin->widget);
        }
      }

      /* lets get next plugin */
      plugins = g_list_previous(plugins);
    }

    /* enter view. crucially, do this before initing the plugins below,
       as e.g. modulegroups requires the dr stuff to be inited. */
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

      if (!plugin->views)
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
  while (plugins && !handled)
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
    return v->button_pressed(v, x, y, which,type,state);

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


void
dt_view_image_expose(
  dt_view_image_over_t *image_over,
  uint32_t imgid,
  cairo_t *cr,
  int32_t width,
  int32_t height,
  int32_t zoom,
  int32_t px,
  int32_t py,
  gboolean full_preview)
{
  const double start = dt_get_wtime();
  // some performance tuning stuff, for your pleasure.
  // on my machine with 7 image per row it seems grouping has the largest
  // impact from around 400ms -> 55ms per redraw.
#define DRAW_THUMB 1
#define DRAW_COLORLABELS 1
#define DRAW_GROUPING 1
#define DRAW_SELECTED 1
#define DRAW_HISTORY 1

#if DRAW_THUMB == 1
  // this function is not thread-safe (gui-thread only), so we
  // can safely allocate this leaking bit of memory to decompress thumbnails:
  static int first_time = 1;
  static uint8_t *scratchmem = NULL;
  if(first_time)
  {
    // scratchmem might still be NULL after this, if compression is off.
    scratchmem = dt_mipmap_cache_alloc_scratchmem(darktable.mipmap_cache);
    first_time = 0;
  }
#endif

  cairo_save (cr);
  float bgcol = 0.4, fontcol = 0.425, bordercol = 0.1, outlinecol = 0.2;
  int selected = 0, altered = 0, imgsel = -1, is_grouped = 0;
  // this is a gui thread only thing. no mutex required:
  imgsel = darktable.control->global_settings.lib_image_mouse_over_id;

#if DRAW_SELECTED == 1
  /* clear and reset statements */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.is_selected);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.is_selected);
  /* bind imgid to prepared statments */
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.is_selected, 1, imgid);
  /* lets check if imgid is selected */
  if(sqlite3_step(darktable.view_manager->statements.is_selected) == SQLITE_ROW)
    selected = 1;
#endif

#if DRAW_HISTORY == 1
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.have_history);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.have_history);
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.have_history, 1, imgid);

  /* lets check if imgid has history */
  if(sqlite3_step(darktable.view_manager->statements.have_history) == SQLITE_ROW)
    altered = 1;
#endif

  const dt_image_t *img = dt_image_cache_read_testget(darktable.image_cache, imgid);

#if DRAW_GROUPING == 1
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.get_grouped);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.get_grouped);
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.get_grouped, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.get_grouped, 2, imgid);

  /* lets check if imgid is in a group */
  if(sqlite3_step(darktable.view_manager->statements.get_grouped) == SQLITE_ROW)
    is_grouped = 1;
  else if(img && darktable.gui->expanded_group_id == img->group_id)
    darktable.gui->expanded_group_id = -1;
#endif

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
    // if the user points at this image, we really want it:
    if(!img)
      img = dt_image_cache_read_get(darktable.image_cache, imgid);
  }
  float imgwd = 0.90f;
  if(zoom == 1)
  {
    imgwd = .97f;
    // cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  }
  else
  {
    double x0 = 1, y0 = 1, rect_width = width-2, rect_height = height-2, radius = 5;
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
      cairo_text_extents_t text_extends;
      cairo_text_extents (cr, ext, &text_extends);
      cairo_move_to (cr, .025*width - text_extends.x_bearing, .24*height);
      cairo_show_text (cr, ext);
    }
  }

  dt_mipmap_buffer_t buf;
  dt_mipmap_size_t mip =
    dt_mipmap_cache_get_matching_size(
      darktable.mipmap_cache,
      imgwd*width, imgwd*height);
  dt_mipmap_cache_read_get(
    darktable.mipmap_cache,
    &buf,
    imgid,
    mip,
    0);
#if DRAW_THUMB == 1
  float scale = 1.0;
  // decompress image, if necessary. if compression is off, scratchmem will be == NULL,
  // so get the real pointer back:
  uint8_t *buf_decompressed = dt_mipmap_cache_decompress(&buf, scratchmem);

  cairo_surface_t *surface = NULL;
  if(buf.buf)
  {
    const int32_t stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, buf.width);
    surface = cairo_image_surface_create_for_data (buf_decompressed, CAIRO_FORMAT_RGB24, buf.width, buf.height, stride);
    if(zoom == 1)
    {
      scale = fminf(
                fminf(darktable.thumbnail_width, width) / (float)buf.width,
                fminf(darktable.thumbnail_height, height) / (float)buf.height
              );
    }
    else scale = fminf(width*imgwd/(float)buf.width, height*imgwd/(float)buf.height);
  }

  // draw centered and fitted:
  cairo_save(cr);
  cairo_translate(cr, width/2.0, height/2.0f);
  cairo_scale(cr, scale, scale);

  if(buf.buf)
  {
    cairo_translate(cr, -.5f*buf.width, -.5f*buf.height);
    cairo_set_source_surface (cr, surface, 0, 0);
    // set filter no nearest:
    // in skull mode, we want to see big pixels.
    // in 1 iir mode for the right mip, we want to see exactly what the pipe gave us, 1:1 pixel for pixel.
    // in between, filtering just makes stuff go unsharp.
    if((buf.width <= 8 && buf.height <= 8) || fabsf(scale - 1.0f) < 0.01f)
      cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_rectangle(cr, 0, 0, buf.width, buf.height);
    cairo_fill(cr);
    cairo_surface_destroy (surface);

    cairo_rectangle(cr, 0, 0, buf.width, buf.height);
  }

  // border around image
  const float border = zoom == 1 ? 16/scale : 2/scale;
  cairo_set_source_rgb(cr, bordercol, bordercol, bordercol);
  if(buf.buf && selected)
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
        cairo_rectangle(cr, 0, 0, buf.width, buf.height);
        cairo_new_sub_path(cr);
        cairo_rectangle(cr, -k/scale, -k/scale, buf.width+2.*k/scale, buf.height+2.*k/scale);
        cairo_set_source_rgba(cr, 0, 0, 0, alpha);
        alpha *= 0.6f;
        cairo_fill(cr);
      }
    }
    else
    {
      cairo_set_fill_rule (cr, CAIRO_FILL_RULE_EVEN_ODD);
      cairo_new_sub_path(cr);
      cairo_rectangle(cr, -border, -border, buf.width+2.*border, buf.height+2.*border);
      cairo_stroke_preserve(cr);
      cairo_set_source_rgb(cr, 1.0-bordercol, 1.0-bordercol, 1.0-bordercol);
      cairo_fill(cr);
    }
  }
  else if(buf.buf)
  {
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
  }
  cairo_restore(cr);
#endif
  if(buf.buf)
    dt_mipmap_cache_read_release(darktable.mipmap_cache, &buf);

  const float fscale = fminf(width, height);
  if(imgsel == imgid || full_preview)
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
    gboolean image_is_rejected = (img && ((img->flags & 0x7) == 6));

    if(img) for(int k=0; k<5; k++)
      {
        if(zoom != 1) x = (0.41+k*0.12)*width;
        else x = (.08+k*0.04)*fscale;

        if(!image_is_rejected) //if rejected: draw no stars
        {
          dt_view_star(cr, x, y, r1, r2);
          if((px - x)*(px - x) + (py - y)*(py - y) < r1*r1)
          {
            *image_over = DT_VIEW_STAR_1 + k;
            cairo_fill(cr);
          }
          else if((img->flags & 0x7) > k)
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

    if (image_is_rejected)
      cairo_set_source_rgb(cr, 1., 0., 0.);

    if((px - x)*(px - x) + (py - y)*(py - y) < r1*r1)
    {
      *image_over = DT_VIEW_REJECT; //mouse sensitive
      cairo_new_sub_path(cr);
      cairo_arc(cr, x, y, (r1+r2)*.5, 0, 2.0f*M_PI);
      cairo_stroke(cr);
    }

    if (image_is_rejected)
      cairo_set_line_width(cr, 2.5);

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
      if(img && (imgid != img->group_id))
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
        darktable.gui->center_tooltip = 1;
      }
    }
  }

  // kill all paths, in case img was not loaded yet, or is blocked:
  cairo_new_path(cr);

#if DRAW_COLORLABELS == 1
  // TODO: make mouse sensitive, just as stars!
  // TODO: cache in image struct!
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
#endif

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

  if(img) dt_image_cache_read_release(darktable.image_cache, img);
  cairo_restore(cr);
  // if(zoom == 1) cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);

  const double end = dt_get_wtime();
  if (darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_LIGHTTABLE, "[lighttable] image expose took %0.04f sec\n", end-start);
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

/**
 * \brief Reset filter back to "all images"
 */
void dt_view_filter_reset_to_show_all(const dt_view_manager_t *vm)
{
  if (vm->proxy.filter.module && vm->proxy.filter.reset_filter)
    vm->proxy.filter.reset_filter(vm->proxy.filter.module);
}

void dt_view_filmstrip_scroll_relative(const int diff, int offset)
{
  const gchar *qin = dt_collection_get_query (darktable.collection);
  if(qin)
  {
    int imgid = -1;
    sqlite3_stmt *stmt;

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), qin, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, offset + diff);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, 1);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      imgid = sqlite3_column_int(stmt, 0);

      if (!darktable.develop->image_loading)
      {
        dt_view_filmstrip_scroll_to_image(darktable.view_manager, imgid, TRUE);
      }
    }
    sqlite3_finalize(stmt);
  }

}

void dt_view_filmstrip_scroll_to_image(dt_view_manager_t *vm, const int imgid, gboolean activate )
{
  //g_return_if_fail(vm->proxy.filmstrip.module!=NULL); // This can happend here for debugging
  //g_return_if_fail(vm->proxy.filmstrip.scroll_to_image!=NULL);

  if(vm->proxy.filmstrip.module && vm->proxy.filmstrip.scroll_to_image)
    vm->proxy.filmstrip.scroll_to_image(vm->proxy.filmstrip.module, imgid, activate);
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

  dt_view_filmstrip_scroll_to_image(vm, iid, TRUE);
}

void dt_view_filmstrip_prefetch()
{
  const gchar *qin = dt_collection_get_query (darktable.collection);
  if(!qin) return;

  int offset = 0;
  if(qin)
  {
    int imgid = -1;
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select imgid from selected_images", -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
      imgid = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    offset = dt_collection_image_offset(imgid);
  }

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), qin, -1, &stmt, NULL);
  // only get one more image:
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, offset+1);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, offset+2);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const uint32_t prefetchid = sqlite3_column_int(stmt, 0);
    // dt_control_log("prefetching image %u", prefetchid);
    dt_mipmap_cache_read_get(darktable.mipmap_cache, NULL, prefetchid, DT_MIPMAP_FULL, DT_MIPMAP_PREFETCH);
  }
  sqlite3_finalize(stmt);
}

void dt_view_manager_view_toolbox_add(dt_view_manager_t *vm,GtkWidget *tool)
{
  if (vm->proxy.view_toolbox.module)
    vm->proxy.view_toolbox.add(vm->proxy.view_toolbox.module,tool);
}

void dt_view_manager_module_toolbox_add(dt_view_manager_t *vm,GtkWidget *tool)
{
  if (vm->proxy.module_toolbox.module)
    vm->proxy.module_toolbox.add(vm->proxy.module_toolbox.module,tool);
}

void dt_view_lighttable_set_zoom(dt_view_manager_t *vm, gint zoom)
{
  if (vm->proxy.lighttable.module)
    vm->proxy.lighttable.set_zoom(vm->proxy.lighttable.module, zoom);
}

void dt_view_collection_update(const dt_view_manager_t *vm)
{
  if (vm->proxy.module_collect.module)
    vm->proxy.module_collect.update(vm->proxy.module_collect.module);
}

int32_t dt_view_tethering_get_film_id(const dt_view_manager_t *vm)
{
  if (vm->proxy.tethering.view)
    return vm->proxy.tethering.get_film_id(vm->proxy.tethering.view);

  return -1;
}

int32_t dt_view_tethering_get_selected_imgid(const dt_view_manager_t *vm)
{
  if (vm->proxy.tethering.view)
    return vm->proxy.tethering.get_selected_imgid(vm->proxy.tethering.view);

  return -1;
}


const char *dt_view_tethering_get_session_path(const dt_view_manager_t *vm)
{
  if (vm->proxy.tethering.view)
    return vm->proxy.tethering.get_session_path(vm->proxy.tethering.view);
  return NULL;
}

const char *dt_view_tethering_get_session_filename(const dt_view_manager_t *vm, const char *filename)
{
  if (vm->proxy.tethering.view)
    return vm->proxy.tethering.get_session_filename(vm->proxy.tethering.view, filename);
  return NULL;
}

void dt_view_tethering_set_job_code(const dt_view_manager_t *vm, const char *name)
{
  if (vm->proxy.tethering.view)
    vm->proxy.tethering.set_job_code(vm->proxy.tethering.view, name);
}

const char *dt_view_tethering_get_job_code(const dt_view_manager_t *vm)
{
  if (vm->proxy.tethering.view)
    return vm->proxy.tethering.get_job_code(vm->proxy.tethering.view);
  return NULL;
}

void dt_view_tethering_set_session_namepattern(const dt_view_manager_t *vm, const char *namepattern)
{
  if (vm->proxy.tethering.view)
    vm->proxy.tethering.set_session_namepattern(vm->proxy.tethering.view, namepattern);
}

gboolean dt_view_tethering_check_namepattern(const dt_view_manager_t *vm)
{
  if (vm->proxy.tethering.view)
    return vm->proxy.tethering.check_namepattern(vm->proxy.tethering.view);
  return FALSE;
}

#ifdef HAVE_MAP
void dt_view_map_center_on_location(const dt_view_manager_t *vm, gdouble lon, gdouble lat, gdouble zoom)
{
  if (vm->proxy.map.view)
    vm->proxy.map.center_on_location(vm->proxy.map.view, lon, lat, zoom);
}

void dt_view_map_show_osd(const dt_view_manager_t *vm, gboolean enabled)
{
  if (vm->proxy.map.view)
    vm->proxy.map.show_osd(vm->proxy.map.view, enabled);
}

void dt_view_map_set_map_source(const dt_view_manager_t *vm, OsmGpsMapSource_t map_source)
{
  if (vm->proxy.map.view)
    vm->proxy.map.set_map_source(vm->proxy.map.view, map_source);
}
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
