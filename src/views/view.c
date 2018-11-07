/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2011-2014 henrik andersson.
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

#include "views/view.h"
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/module.h"
#include "common/undo.h"
#include "common/usermanual_url.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/expander.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DECORATION_SIZE_LIMIT 40

static void dt_view_manager_load_modules(dt_view_manager_t *vm);
static int dt_view_load_module(void *v, const char *libname, const char *module_name);
static void dt_view_unload_module(dt_view_t *view);

void dt_view_manager_init(dt_view_manager_t *vm)
{
  /* prepare statements */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images "
                              "WHERE imgid = ?1", -1, &vm->statements.is_selected, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.selected_images WHERE imgid = ?1",
                              -1, &vm->statements.delete_from_selected, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT OR IGNORE INTO main.selected_images VALUES (?1)", -1,
                              &vm->statements.make_selected, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT num FROM main.history WHERE imgid = ?1", -1,
                              &vm->statements.have_history, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT color FROM main.color_labels WHERE imgid=?1",
                              -1, &vm->statements.get_color, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT id FROM main.images WHERE group_id = (SELECT group_id FROM main.images WHERE id=?1) AND id != ?2",
      -1, &vm->statements.get_grouped, NULL);

  dt_view_manager_load_modules(vm);

  // Modules loaded, let's handle specific cases
  for(GList *iter = vm->views; iter; iter = g_list_next(iter))
  {
    dt_view_t *view = (dt_view_t *)iter->data;
    if(!strcmp(view->module_name, "darkroom"))
    {
      darktable.develop = (dt_develop_t *)view->data;
      break;
    }
  }

  vm->current_view = NULL;
}

void dt_view_manager_gui_init(dt_view_manager_t *vm)
{
  for(GList *iter = vm->views; iter; iter = g_list_next(iter))
  {
    dt_view_t *view = (dt_view_t *)iter->data;
    if(view->gui_init) view->gui_init(view);
  }
}

void dt_view_manager_cleanup(dt_view_manager_t *vm)
{
  for(GList *iter = vm->views; iter; iter = g_list_next(iter)) dt_view_unload_module((dt_view_t *)iter->data);
}

const dt_view_t *dt_view_manager_get_current_view(dt_view_manager_t *vm)
{
  return vm->current_view;
}

// we want a stable order of views, for example for viewswitcher.
// anything not hardcoded will be put alphabetically wrt. localised names.
static gint sort_views(gconstpointer a, gconstpointer b)
{
  static const char *view_order[] = {"lighttable", "darkroom"};
  static const int n_view_order = G_N_ELEMENTS(view_order);

  dt_view_t *av = (dt_view_t *)a;
  dt_view_t *bv = (dt_view_t *)b;
  const char *aname = av->name(av);
  const char *bname = bv->name(bv);
  int apos = n_view_order;
  int bpos = n_view_order;

  for(int i = 0; i < n_view_order; i++)
  {
    if(!strcmp(av->module_name, view_order[i])) apos = i;
    if(!strcmp(bv->module_name, view_order[i])) bpos = i;
  }

  // order will be zero iff apos == bpos which can only happen when both views are not in view_order
  const int order = apos - bpos;
  return order ? order : strcmp(aname, bname);
}

static void dt_view_manager_load_modules(dt_view_manager_t *vm)
{
  vm->views = dt_module_load_modules("/views", sizeof(dt_view_t), dt_view_load_module, NULL, sort_views);
}

/* default flags for view which does not implement the flags() function */
static uint32_t default_flags()
{
  return 0;
}

/** load a view module */
static int dt_view_load_module(void *v, const char *libname, const char *module_name)
{
  dt_view_t *view = (dt_view_t *)v;

  view->data = NULL;
  view->vscroll_size = view->vscroll_viewport_size = 1.0;
  view->hscroll_size = view->hscroll_viewport_size = 1.0;
  view->vscroll_pos = view->hscroll_pos = 0.0;
  view->height = view->width = 100; // set to non-insane defaults before first expose/configure.
  g_strlcpy(view->module_name, module_name, sizeof(view->module_name));
  dt_print(DT_DEBUG_CONTROL, "[view_load_module] loading view `%s' from %s\n", module_name, libname);
  view->module = g_module_open(libname, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
  if(!view->module)
  {
    fprintf(stderr, "[view_load_module] could not open %s (%s)!\n", libname, g_module_error());
    goto error;
  }
  int (*version)();
  if(!g_module_symbol(view->module, "dt_module_dt_version", (gpointer) & (version))) goto error;
  if(version() != dt_version())
  {
    fprintf(stderr, "[view_load_module] `%s' is compiled for another version of dt (module %d != dt %d) !\n",
            libname, version(), dt_version());
    goto error;
  }
  if(!g_module_symbol(view->module, "name", (gpointer) & (view->name))) view->name = NULL;
  if(!g_module_symbol(view->module, "view", (gpointer) & (view->view))) view->view = NULL;
  if(!g_module_symbol(view->module, "flags", (gpointer) & (view->flags))) view->flags = default_flags;
  if(!g_module_symbol(view->module, "init", (gpointer) & (view->init))) view->init = NULL;
  if(!g_module_symbol(view->module, "gui_init", (gpointer) & (view->gui_init))) view->gui_init = NULL;
  if(!g_module_symbol(view->module, "cleanup", (gpointer) & (view->cleanup))) view->cleanup = NULL;
  if(!g_module_symbol(view->module, "expose", (gpointer) & (view->expose))) view->expose = NULL;
  if(!g_module_symbol(view->module, "try_enter", (gpointer) & (view->try_enter))) view->try_enter = NULL;
  if(!g_module_symbol(view->module, "enter", (gpointer) & (view->enter))) view->enter = NULL;
  if(!g_module_symbol(view->module, "leave", (gpointer) & (view->leave))) view->leave = NULL;
  if(!g_module_symbol(view->module, "reset", (gpointer) & (view->reset))) view->reset = NULL;
  if(!g_module_symbol(view->module, "mouse_enter", (gpointer) & (view->mouse_enter)))
    view->mouse_enter = NULL;
  if(!g_module_symbol(view->module, "mouse_leave", (gpointer) & (view->mouse_leave)))
    view->mouse_leave = NULL;
  if(!g_module_symbol(view->module, "mouse_moved", (gpointer) & (view->mouse_moved)))
    view->mouse_moved = NULL;
  if(!g_module_symbol(view->module, "button_released", (gpointer) & (view->button_released)))
    view->button_released = NULL;
  if(!g_module_symbol(view->module, "button_pressed", (gpointer) & (view->button_pressed)))
    view->button_pressed = NULL;
  if(!g_module_symbol(view->module, "key_pressed", (gpointer) & (view->key_pressed)))
    view->key_pressed = NULL;
  if(!g_module_symbol(view->module, "key_released", (gpointer) & (view->key_released)))
    view->key_released = NULL;
  if(!g_module_symbol(view->module, "configure", (gpointer) & (view->configure))) view->configure = NULL;
  if(!g_module_symbol(view->module, "scrolled", (gpointer) & (view->scrolled))) view->scrolled = NULL;
  if(!g_module_symbol(view->module, "scrollbar_changed", (gpointer) & (view->scrollbar_changed)))
    view->scrollbar_changed = NULL;
  if(!g_module_symbol(view->module, "init_key_accels", (gpointer) & (view->init_key_accels)))
    view->init_key_accels = NULL;
  if(!g_module_symbol(view->module, "connect_key_accels", (gpointer) & (view->connect_key_accels)))
    view->connect_key_accels = NULL;

  view->accel_closures = NULL;

  if(!strcmp(view->module_name, "darkroom")) darktable.develop = (dt_develop_t *)view->data;

#ifdef USE_LUA
  dt_lua_register_view(darktable.lua_state.state, view);
#endif

  if(view->init) view->init(view);
  if(darktable.gui && view->init_key_accels) view->init_key_accels(view);

  return 0;

error:
  if(view->module) g_module_close(view->module);
  return 1;
}

/** unload, cleanup */
static void dt_view_unload_module(dt_view_t *view)
{
  if(view->cleanup) view->cleanup(view);

  g_slist_free(view->accel_closures);

  if(view->module) g_module_close(view->module);
}

void dt_vm_remove_child(GtkWidget *widget, gpointer data)
{
  gtk_container_remove(GTK_CONTAINER(data), widget);
}

/*
   When expanders get destoyed, they destroy the child
   so remove the child before that
   */
static void _remove_child(GtkWidget *child,GtkContainer *container)
{
    if(DTGTK_IS_EXPANDER(child))
    {
      GtkWidget * evb = dtgtk_expander_get_body_event_box(DTGTK_EXPANDER(child));
      gtk_container_remove(GTK_CONTAINER(evb),dtgtk_expander_get_body(DTGTK_EXPANDER(child)));
      gtk_widget_destroy(child);
    }
    else
    {
      gtk_container_remove(container,child);
    }
}

static void bitness_nagging()
{
  const int bits = (sizeof(void *) == 4) ? 32 : 64;
  if((bits < 64) && !dt_conf_get_bool("please_let_me_suffer_by_using_32bit_darktable"))
  {
    fprintf(stderr, "warning: 32-bit build!\n");

    GtkWidget *dialog, *content_area;
    GtkDialogFlags flags;

    // Create the widgets
    flags = GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT;
    dialog = gtk_dialog_new_with_buttons(
        _("you are making a mistake!"), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), flags,
        _("_yes, i understood. please let me suffer by using 32-bit darktable."), GTK_RESPONSE_NONE,
        NULL);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif
    content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

    const gchar *msg = _("warning!\nyou are using a 32-bit build of darktable.\nthe 32-bit build has "
                          "severely limited virtual address space.\nwe have had numerous reports that "
                          "darktable exhibits sporadic issues and crashes when using 32-bit builds.\nwe "
                          "strongly recommend you switch to a proper 64-bit build.\notherwise, you are "
                          "GUARANTEED to experience issues which cannot be fixed.\n");

    gtk_container_add(GTK_CONTAINER(content_area), gtk_label_new(msg));
    gtk_widget_show_all(dialog);

    (void)gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }
}

int dt_view_manager_switch(dt_view_manager_t *vm, const char *view_name)
{
  gboolean switching_to_none = *view_name == '\0';
  dt_view_t *new_view = NULL;

  if(!switching_to_none)
  {
    for(GList *iter = vm->views; iter; iter = g_list_next(iter))
    {
      dt_view_t *v = (dt_view_t *)iter->data;
      if(!strcmp(v->module_name, view_name))
      {
        new_view = v;
        break;
      }
    }
    if(!new_view) return 1; // the requested view doesn't exist
  }

  return dt_view_manager_switch_by_view(vm, new_view);
}

int dt_view_manager_switch_by_view(dt_view_manager_t *vm, const dt_view_t *nv)
{
  dt_view_t *old_view = vm->current_view;
  dt_view_t *new_view = (dt_view_t *)nv; // views belong to us, we can de-const them :-)

  // Before switching views, restore accelerators if disabled
  if(!darktable.control->key_accelerators_on) dt_control_key_accelerators_on(darktable.control);

  // reset the cursor to the default one
  dt_control_change_cursor(GDK_LEFT_PTR);

  // also ignore what scrolling there was previously happening
  memset(darktable.gui->scroll_to, 0, sizeof(darktable.gui->scroll_to));

  // destroy old module list

  /*  clear the undo list, for now we do this inconditionally. At some point we will probably want to clear
     only part
      of the undo list. This should probably done with a view proxy routine returning the type of undo to
     remove. */
  dt_undo_clear(darktable.undo, DT_UNDO_ALL);

  /* Special case when entering nothing (just before leaving dt) */
  if(!new_view)
  {
    if(old_view)
    {
      /* leave the current view*/
      if(old_view->leave) old_view->leave(old_view);

      /* iterator plugins and cleanup plugins in current view */
      for(GList *iter = darktable.lib->plugins; iter; iter = g_list_next(iter))
      {
        dt_lib_module_t *plugin = (dt_lib_module_t *)(iter->data);

        /* does this module belong to current view ?*/
        if(dt_lib_is_visible_in_view(plugin, old_view))
        {
          if(plugin->view_leave) plugin->view_leave(plugin, old_view, NULL);
          plugin->gui_cleanup(plugin);
          plugin->data = NULL;
          dt_accel_disconnect_list(plugin->accel_closures);
          plugin->accel_closures = NULL;
          plugin->widget = NULL;
        }
      }
    }

    /* remove all widgets in all containers */
    for(int l = 0; l < DT_UI_CONTAINER_SIZE; l++)
      dt_ui_container_destroy_children(darktable.gui->ui, l);
    vm->current_view = NULL;
    return 0;
  }

  // invariant: new_view != NULL after this point
  assert(new_view != NULL);

  if(new_view->try_enter)
  {
    int error = new_view->try_enter(new_view);
    if(error) return error;
  }

  // annoy the users that are still on 32 bit systems!
  bitness_nagging();

  /* cleanup current view before initialization of new  */
  if(old_view)
  {
    /* leave current view */
    if(old_view->leave) old_view->leave(old_view);
    dt_accel_disconnect_list(old_view->accel_closures);
    old_view->accel_closures = NULL;

    /* iterator plugins and cleanup plugins in current view */
    for(GList *iter = darktable.lib->plugins; iter; iter = g_list_next(iter))
    {
      dt_lib_module_t *plugin = (dt_lib_module_t *)(iter->data);

      /* does this module belong to current view ?*/
      if(dt_lib_is_visible_in_view(plugin, old_view))
      {
        if(plugin->view_leave) plugin->view_leave(plugin, old_view, new_view);
        dt_accel_disconnect_list(plugin->accel_closures);
        plugin->accel_closures = NULL;
      }
    }

    /* remove all widets in all containers */
    for(int l = 0; l < DT_UI_CONTAINER_SIZE; l++)
      dt_ui_container_foreach(darktable.gui->ui, l,(GtkCallback)_remove_child);
  }

  /* change current view to the new view */
  vm->current_view = new_view;

  /* restore visible stat of panels for the new view */
  dt_ui_restore_panels(darktable.gui->ui);

  /* lets add plugins related to new view into panels.
   * this has to be done in reverse order to have the lowest position at the bottom! */
  for(GList *iter = g_list_last(darktable.lib->plugins); iter; iter = g_list_previous(iter))
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(iter->data);
    if(dt_lib_is_visible_in_view(plugin, new_view))
    {

      /* try get the module expander  */
      GtkWidget *w = dt_lib_gui_get_expander(plugin);

      if(plugin->connect_key_accels) plugin->connect_key_accels(plugin);
      dt_lib_connect_common_accels(plugin);

      /* if we didn't get an expander let's add the widget */
      if(!w) w = plugin->widget;

      dt_gui_add_help_link(w, dt_get_help_url(plugin->plugin_name));
      // some plugins help links depend on the view
      if(!strcmp(plugin->plugin_name,"module_toolbox")
        || !strcmp(plugin->plugin_name,"view_toolbox"))
      {
        dt_view_type_flags_t view_type = new_view->view(new_view);
        if(view_type == DT_VIEW_LIGHTTABLE)
          dt_gui_add_help_link(w,"lighttable_chapter.html#lighttable_overview");
        if(view_type == DT_VIEW_DARKROOM)
          dt_gui_add_help_link(w,"darkroom_bottom_panel.html#darkroom_bottom_panel");
      }


      /* add module to its container */
      dt_ui_container_add_widget(darktable.gui->ui, plugin->container(plugin), w);
    }
  }

  /* hide/show modules as last config */
  for(GList *iter = darktable.lib->plugins; iter; iter = g_list_next(iter))
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(iter->data);
    if(dt_lib_is_visible_in_view(plugin, new_view))
    {
      /* set expanded if last mode was that */
      char var[1024];
      gboolean expanded = FALSE;
      gboolean visible = dt_lib_is_visible(plugin);
      if(plugin->expandable(plugin))
      {
        snprintf(var, sizeof(var), "plugins/%s/%s/expanded", new_view->module_name, plugin->plugin_name);
        expanded = dt_conf_get_bool(var);

        dt_lib_gui_set_expanded(plugin, expanded);
      }
      else
      {
        /* show/hide plugin widget depending on expanded flag or if plugin
            not is expandeable() */
        if(visible)
          gtk_widget_show_all(plugin->widget);
        else
          gtk_widget_hide(plugin->widget);
      }
      if(plugin->view_enter) plugin->view_enter(plugin, old_view, new_view);
    }
  }

  /* enter view. crucially, do this before initing the plugins below,
      as e.g. modulegroups requires the dr stuff to be inited. */
  if(new_view->enter) new_view->enter(new_view);
  if(new_view->connect_key_accels) new_view->connect_key_accels(new_view);

  /* update the scrollbars */
  dt_ui_update_scrollbars(darktable.gui->ui);

  /* raise view changed signal */
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED, old_view, new_view);

  /* add endmarkers to left and right center containers */
  GtkWidget *endmarker = gtk_drawing_area_new();
  dt_ui_container_add_widget(darktable.gui->ui, DT_UI_CONTAINER_PANEL_LEFT_CENTER, endmarker);
  g_signal_connect(G_OBJECT(endmarker), "draw", G_CALLBACK(dt_control_draw_endmarker), 0);
  gtk_widget_set_size_request(endmarker, -1, DT_PIXEL_APPLY_DPI(50));
  gtk_widget_show(endmarker);

  endmarker = gtk_drawing_area_new();
  dt_ui_container_add_widget(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER, endmarker);
  g_signal_connect(G_OBJECT(endmarker), "draw", G_CALLBACK(dt_control_draw_endmarker), GINT_TO_POINTER(1));
  gtk_widget_set_size_request(endmarker, -1, DT_PIXEL_APPLY_DPI(50));
  gtk_widget_show(endmarker);

  return 0;
}

const char *dt_view_manager_name(dt_view_manager_t *vm)
{
  if(!vm->current_view) return "";
  if(vm->current_view->name)
    return vm->current_view->name(vm->current_view);
  else
    return vm->current_view->module_name;
}

void dt_view_manager_expose(dt_view_manager_t *vm, cairo_t *cr, int32_t width, int32_t height,
                            int32_t pointerx, int32_t pointery)
{
  if(!vm->current_view)
  {
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_BG);
    cairo_paint(cr);
    return;
  }
  vm->current_view->width = width;
  vm->current_view->height = height;

  if(vm->current_view->expose)
  {
    /* expose the view */
    cairo_rectangle(cr, 0, 0, vm->current_view->width, vm->current_view->height);
    cairo_clip(cr);
    cairo_new_path(cr);
    cairo_save(cr);
    float px = pointerx, py = pointery;
    if(pointery > vm->current_view->height)
    {
      px = 10000.0;
      py = -1.0;
    }
    vm->current_view->expose(vm->current_view, cr, vm->current_view->width, vm->current_view->height, px, py);

    cairo_restore(cr);
    /* expose plugins */
    GList *plugins = g_list_last(darktable.lib->plugins);
    while(plugins)
    {
      dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

      /* does this module belong to current view ?*/
      if(plugin->gui_post_expose && dt_lib_is_visible_in_view(plugin, vm->current_view))
        plugin->gui_post_expose(plugin, cr, vm->current_view->width, vm->current_view->height, px, py);

      /* get next plugin */
      plugins = g_list_previous(plugins);
    }
  }
}

void dt_view_manager_reset(dt_view_manager_t *vm)
{
  if(!vm->current_view) return;
  if(vm->current_view->reset) vm->current_view->reset(vm->current_view);
}

void dt_view_manager_mouse_leave(dt_view_manager_t *vm)
{
  if(!vm->current_view) return;
  dt_view_t *v = vm->current_view;

  /* lets check if any plugins want to handle mouse move */
  gboolean handled = FALSE;
  GList *plugins = g_list_last(darktable.lib->plugins);
  while(plugins)
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

    /* does this module belong to current view ?*/
    if(plugin->mouse_leave && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->mouse_leave(plugin)) handled = TRUE;

    /* get next plugin */
    plugins = g_list_previous(plugins);
  }

  /* if not handled by any plugin let pass to view handler*/
  if(!handled && v->mouse_leave) v->mouse_leave(v);
}

void dt_view_manager_mouse_enter(dt_view_manager_t *vm)
{
  if(!vm->current_view) return;
  if(vm->current_view->mouse_enter) vm->current_view->mouse_enter(vm->current_view);
}

void dt_view_manager_mouse_moved(dt_view_manager_t *vm, double x, double y, double pressure, int which)
{
  if(!vm->current_view) return;
  dt_view_t *v = vm->current_view;

  /* lets check if any plugins want to handle mouse move */
  gboolean handled = FALSE;
  GList *plugins = g_list_last(darktable.lib->plugins);
  while(plugins)
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

    /* does this module belong to current view ?*/
    if(plugin->mouse_moved && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->mouse_moved(plugin, x, y, pressure, which)) handled = TRUE;

    /* get next plugin */
    plugins = g_list_previous(plugins);
  }

  /* if not handled by any plugin let pass to view handler*/
  if(!handled && v->mouse_moved) v->mouse_moved(v, x, y, pressure, which);
}

int dt_view_manager_button_released(dt_view_manager_t *vm, double x, double y, int which, uint32_t state)
{
  if(!vm->current_view) return 0;
  dt_view_t *v = vm->current_view;

  /* lets check if any plugins want to handle button press */
  gboolean handled = FALSE;
  GList *plugins = g_list_last(darktable.lib->plugins);
  while(plugins)
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

    /* does this module belong to current view ?*/
    if(plugin->button_released && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->button_released(plugin, x, y, which, state)) handled = TRUE;

    /* get next plugin */
    plugins = g_list_previous(plugins);
  }

  /* if not handled by any plugin let pass to view handler*/
  if(!handled && v->button_released) v->button_released(v, x, y, which, state);

  return 0;
}

int dt_view_manager_button_pressed(dt_view_manager_t *vm, double x, double y, double pressure, int which,
                                   int type, uint32_t state)
{
  if(!vm->current_view) return 0;
  dt_view_t *v = vm->current_view;

  /* lets check if any plugins want to handle button press */
  gboolean handled = FALSE;
  GList *plugins = g_list_last(darktable.lib->plugins);
  while(plugins && !handled)
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(plugins->data);

    /* does this module belong to current view ?*/
    if(plugin->button_pressed && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->button_pressed(plugin, x, y, pressure, which, type, state)) handled = TRUE;

    /* get next plugin */
    plugins = g_list_previous(plugins);
  }

  /* if not handled by any plugin let pass to view handler*/
  if(!handled && v->button_pressed) return v->button_pressed(v, x, y, pressure, which, type, state);

  return 0;
}

int dt_view_manager_key_pressed(dt_view_manager_t *vm, guint key, guint state)
{
  // ↑ ↑ ↓ ↓ ← → ← → b a
  static int konami_state = 0;
  static guint konami_sequence[] = {
    GDK_KEY_Up,
    GDK_KEY_Up,
    GDK_KEY_Down,
    GDK_KEY_Down,
    GDK_KEY_Left,
    GDK_KEY_Right,
    GDK_KEY_Left,
    GDK_KEY_Right,
    GDK_KEY_b,
    GDK_KEY_a
  };
  if(key == konami_sequence[konami_state])
  {
    konami_state++;
    if(konami_state == G_N_ELEMENTS(konami_sequence))
    {
      dt_ctl_switch_mode_to("knight");
      konami_state = 0;
    }
  }
  else
    konami_state = 0;

  int film_strip_result = 0;
  if(!vm->current_view) return 0;
  if(vm->current_view->key_pressed)
    return vm->current_view->key_pressed(vm->current_view, key, state) || film_strip_result;
  return 0;
}

int dt_view_manager_key_released(dt_view_manager_t *vm, guint key, guint state)
{
  int film_strip_result = 0;
  if(!vm->current_view) return 0;
  if(vm->current_view->key_released)
    return vm->current_view->key_released(vm->current_view, key, state) || film_strip_result;
  return 0;
}

void dt_view_manager_configure(dt_view_manager_t *vm, int width, int height)
{
  for(GList *iter = vm->views; iter; iter = g_list_next(iter))
  {
    // this is necessary for all
    dt_view_t *v = (dt_view_t *)iter->data;
    v->width = width;
    v->height = height;
    if(v->configure) v->configure(v, width, height);
  }
}

void dt_view_manager_scrolled(dt_view_manager_t *vm, double x, double y, int up, int state)
{
  if(!vm->current_view) return;
  if(vm->current_view->scrolled) vm->current_view->scrolled(vm->current_view, x, y, up, state);
}

void dt_view_manager_scrollbar_changed(dt_view_manager_t *vm, double x, double y)
{
  if(!vm->current_view) return;
  if(vm->current_view->scrollbar_changed) vm->current_view->scrollbar_changed(vm->current_view, x, y);
}

void dt_view_set_scrollbar(dt_view_t *view, float hpos, float hlower, float hsize, float hwinsize,
                           float vpos, float vlower, float vsize,float vwinsize)
{
  if (view->vscroll_pos == vpos && view->vscroll_lower == vlower && view->vscroll_size == vsize &&
      view->vscroll_viewport_size == vwinsize && view->hscroll_pos == hpos && view->hscroll_lower == hlower &&
      view->hscroll_size == hsize && view->hscroll_viewport_size == hwinsize)
    return;

  view->vscroll_pos = vpos;
  view->vscroll_lower = vlower;
  view->vscroll_size = vsize;
  view->vscroll_viewport_size = vwinsize;
  view->hscroll_pos = hpos;
  view->hscroll_lower = hlower;
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

  if (!darktable.gui->scrollbars.dragging) dt_ui_update_scrollbars(darktable.gui->ui);

}

static inline void dt_view_draw_altered(cairo_t *cr, const float x, const float y, const float r)
{
  cairo_new_sub_path(cr);
  cairo_arc(cr, x, y, r, 0, 2.0f * M_PI);
  const float dx = r * cosf(M_PI / 8.0f), dy = r * sinf(M_PI / 8.0f);
  cairo_move_to(cr, x - dx, y - dy);
  cairo_curve_to(cr, x, y - 2 * dy, x, y + 2 * dy, x + dx, y + dy);
  cairo_move_to(cr, x - .20 * dx, y + .8 * dy);
  cairo_line_to(cr, x - .80 * dx, y + .8 * dy);
  cairo_move_to(cr, x + .20 * dx, y - .8 * dy);
  cairo_line_to(cr, x + .80 * dx, y - .8 * dy);
  cairo_move_to(cr, x + .50 * dx, y - .8 * dy - 0.3 * dx);
  cairo_line_to(cr, x + .50 * dx, y - .8 * dy + 0.3 * dx);
  cairo_stroke(cr);
}

static inline void dt_view_draw_audio(cairo_t *cr, const float x, const float y, const float r)
{
  const float d = 2.0 * r;

  cairo_save(cr);

  cairo_translate(cr, x - (d / 2.0), y - (d / 2.0));
  cairo_scale(cr, d, d);

  cairo_rectangle(cr, 0.05, 0.4, 0.2, 0.2);
  cairo_move_to(cr, 0.25, 0.6);
  cairo_line_to(cr, 0.45, 0.77);
  cairo_line_to(cr, 0.45, 0.23);
  cairo_line_to(cr, 0.25, 0.4);

  cairo_new_sub_path(cr);
  cairo_arc(cr, 0.2, 0.5, 0.45, -(35.0 / 180.0) * M_PI, (35.0 / 180.0) * M_PI);
  cairo_new_sub_path(cr);
  cairo_arc(cr, 0.2, 0.5, 0.6, -(35.0 / 180.0) * M_PI, (35.0 / 180.0) * M_PI);
  cairo_new_sub_path(cr);
  cairo_arc(cr, 0.2, 0.5, 0.75, -(35.0 / 180.0) * M_PI, (35.0 / 180.0) * M_PI);

  cairo_restore(cr);
  cairo_stroke(cr);
}

static inline void dt_view_star(cairo_t *cr, float x, float y, float r1, float r2)
{
  const float d = 2.0 * M_PI * 0.1f;
  const float dx[10] = { sinf(0.0),   sinf(d),     sinf(2 * d), sinf(3 * d), sinf(4 * d),
                         sinf(5 * d), sinf(6 * d), sinf(7 * d), sinf(8 * d), sinf(9 * d) };
  const float dy[10] = { cosf(0.0),   cosf(d),     cosf(2 * d), cosf(3 * d), cosf(4 * d),
                         cosf(5 * d), cosf(6 * d), cosf(7 * d), cosf(8 * d), cosf(9 * d) };
  cairo_move_to(cr, x + r1 * dx[0], y - r1 * dy[0]);
  for(int k = 1; k < 10; k++)
    if(k & 1)
      cairo_line_to(cr, x + r2 * dx[k], y - r2 * dy[k]);
    else
      cairo_line_to(cr, x + r1 * dx[k], y - r1 * dy[k]);
  cairo_close_path(cr);
}

int32_t dt_view_get_image_to_act_on()
{
  // this works as follows:
  // - if mouse hovers over an image, that's the one, except:
  // - if images are selected and the mouse hovers over the selection,
  //   in which case it affects the whole selection.
  // - if the mouse is outside the center view (or no image hovered over otherwise)
  //   it only affects the selection.
  const int32_t mouse_over_id = dt_control_get_mouse_over_id();

  const int zoom = darktable.view_manager->proxy.lighttable.get_images_in_row(
      darktable.view_manager->proxy.lighttable.view);

  const int full_preview_id = darktable.view_manager->proxy.lighttable.get_full_preview_id(
      darktable.view_manager->proxy.lighttable.view);

  if(zoom == 1 || full_preview_id > 1)
  {
    return mouse_over_id;
  }
  else
  {
    /* clear and reset statement */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.is_selected);
    DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.is_selected);

    /* setup statement and iterate over rows */
    DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.is_selected, 1, mouse_over_id);

    if(mouse_over_id <= 0 || sqlite3_step(darktable.view_manager->statements.is_selected) == SQLITE_ROW)
      return -1;
    else
      return mouse_over_id;
  }
}

int dt_view_image_expose(dt_view_image_over_t *image_over, uint32_t imgid, cairo_t *cr, int32_t width,
                         int32_t height, int32_t zoom, int32_t px, int32_t py, gboolean full_preview, gboolean image_only)
{
  int missing = 0;
  const double start = dt_get_wtime();
// some performance tuning stuff, for your pleasure.
// on my machine with 7 image per row it seems grouping has the largest
// impact from around 400ms -> 55ms per redraw.

  // active if zoom>1 or in the proper area
  const gboolean in_metadata_zone = (px < width && py < height / 2) || (zoom > 1);

  const gboolean draw_thumb = TRUE;
  const gboolean draw_colorlabels = !image_only && (darktable.gui->show_overlays || in_metadata_zone);
  const gboolean draw_local_copy = !image_only && (darktable.gui->show_overlays || in_metadata_zone);
  const gboolean draw_grouping = !image_only;
  const gboolean draw_selected = !image_only;
  const gboolean draw_history = !image_only;
  const gboolean draw_metadata = !image_only && (darktable.gui->show_overlays || in_metadata_zone);
  const gboolean draw_audio = !image_only;

  cairo_save(cr);
  float bgcol = 0.4, fontcol = 0.425, bordercol = 0.1, outlinecol = 0.2;
  int selected = 0, is_grouped = 0;
  // this is a gui thread only thing. no mutex required:
  const int imgsel = dt_control_get_mouse_over_id(); //  darktable.control->global_settings.lib_image_mouse_over_id;

  if (draw_selected)
  {
    /* clear and reset statements */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.is_selected);
    DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.is_selected);
    /* bind imgid to prepared statements */
    DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.is_selected, 1, imgid);
    /* lets check if imgid is selected */
    if(sqlite3_step(darktable.view_manager->statements.is_selected) == SQLITE_ROW) selected = 1;
  }

  dt_image_t buffered_image;
  const dt_image_t *img;
  // if darktable.gui->show_overlays is set or the user points at this image, we really want it:
  if(darktable.gui->show_overlays || imgsel == imgid || zoom == 1)
    img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  else
    img = dt_image_cache_testget(darktable.image_cache, imgid, 'r');

  if(selected == 1 && zoom != 1) // If zoom == 1 there is no need to set colors here
  {
    outlinecol = 0.4;
    bgcol = 0.6;
    fontcol = 0.5;
  }
  if(imgsel == imgid || zoom == 1)
  {
    bgcol = 0.8; // mouse over
    fontcol = 0.7;
    outlinecol = 0.6;
  }
  // release image cache lock as early as possible, to avoid deadlocks (mipmap cache might need to lock it, too)
  if(img)
  {
    buffered_image = *img;
    dt_image_cache_read_release(darktable.image_cache, img);
    img = &buffered_image;
  }

  gboolean draw_thumb_background = FALSE;
  float imgwd = 0.90f;
  if (image_only)
  {
    imgwd = 1.0;
  }
  else if(zoom == 1)
  {
    imgwd = .97f;
    // cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  }
  else
  {
    draw_thumb_background = TRUE;
  }

  dt_mipmap_buffer_t buf;
  dt_mipmap_size_t mip
      = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, imgwd * width, imgwd * height);
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, mip, DT_MIPMAP_BEST_EFFORT, 'r');

  if(draw_thumb_background)
  {
    double x0 = DT_PIXEL_APPLY_DPI(1), y0 = DT_PIXEL_APPLY_DPI(1), rect_width = width - DT_PIXEL_APPLY_DPI(2),
           rect_height = height - DT_PIXEL_APPLY_DPI(2), radius = DT_PIXEL_APPLY_DPI(5);
    double x1, y1, off, off1;

    x1 = x0 + rect_width;
    y1 = y0 + rect_height;
    off = radius * 0.666;
    off1 = radius - off;
    cairo_move_to(cr, x0, y0 + radius);
    cairo_curve_to(cr, x0, y0 + off1, x0 + off1, y0, x0 + radius, y0);
    cairo_line_to(cr, x1 - radius, y0);
    cairo_curve_to(cr, x1 - off1, y0, x1, y0 + off1, x1, y0 + radius);
    cairo_line_to(cr, x1, y1 - radius);
    cairo_curve_to(cr, x1, y1 - off1, x1 - off1, y1, x1 - radius, y1);
    cairo_line_to(cr, x0 + radius, y1);
    cairo_curve_to(cr, x0 + off1, y1, x0, y1 - off1, x0, y1 - radius);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, bgcol, bgcol, bgcol);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 0.005 * width);
    cairo_set_source_rgb(cr, outlinecol, outlinecol, outlinecol);
    cairo_stroke(cr);

    if(img)
    {
      PangoLayout *layout;
      PangoRectangle ink;
      PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
      pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
      const int fontsize = 0.20 * width;
      pango_font_description_set_absolute_size(desc, fontsize * PANGO_SCALE);
      layout = pango_cairo_create_layout(cr);
      pango_layout_set_font_description(layout, desc);
      const char *ext = img->filename + strlen(img->filename);
      while(ext > img->filename && *ext != '.') ext--;
      ext++;
      cairo_set_source_rgb(cr, fontcol, fontcol, fontcol);

      char* upcase_ext = g_ascii_strup(ext, -1);  // extension in capital letters to avoid character descenders

      if (buf.height > buf.width)
      {
        int max_chr_width = 0;
        for (int i = 0; upcase_ext[i] != 0; i++)
        {
          pango_layout_set_text(layout, &upcase_ext[i], 1);
          pango_layout_get_pixel_extents(layout, &ink, NULL);
          max_chr_width = MAX(max_chr_width, ink.width);
        }

        for (int i = 0, yoffs = fontsize;  upcase_ext[i] != 0; i++,  yoffs -= fontsize)
        {
          pango_layout_set_text(layout, &upcase_ext[i], 1);
          pango_layout_get_pixel_extents(layout, &ink, NULL);
          cairo_move_to(cr, .025 * width - ink.x + (max_chr_width - ink.width) / 2, .2 * height - yoffs);
          pango_cairo_show_layout(cr, layout);
        }
      }
      else
      {
        pango_layout_set_text(layout, upcase_ext, -1);
        pango_layout_get_pixel_extents(layout, &ink, NULL);
        cairo_move_to(cr, .025 * width - ink.x, .2 * height - fontsize);
        pango_cairo_show_layout(cr, layout);
      }
      g_free(upcase_ext);
      pango_font_description_free(desc);
      g_object_unref(layout);

    }
  }

  // if we got a different mip than requested, and it's not a skull (8x8 px), we count
  // this thumbnail as missing (to trigger re-exposure)
  if(buf.size != mip && buf.width != 8 && buf.height != 8) missing = 1;

  if (draw_thumb)
  {
    float scale = 1.0;

    cairo_surface_t *surface = NULL;
    uint8_t *rgbbuf = NULL;
    if(buf.buf)
    {
      rgbbuf = (uint8_t *)calloc(buf.width * buf.height * 4, sizeof(uint8_t));
      if(rgbbuf)
      {
        gboolean have_lock = FALSE;
        cmsHTRANSFORM transform = NULL;

        if(dt_conf_get_bool("cache_color_managed"))
        {
          pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
          have_lock = TRUE;

          // we only color manage when a thumbnail is sRGB or AdobeRGB. everything else just gets dumped to the screen
          if(buf.color_space == DT_COLORSPACE_SRGB &&
             darktable.color_profiles->transform_srgb_to_display)
          {
            transform = darktable.color_profiles->transform_srgb_to_display;
          }
          else if(buf.color_space == DT_COLORSPACE_ADOBERGB &&
                  darktable.color_profiles->transform_adobe_rgb_to_display)
          {
            transform = darktable.color_profiles->transform_adobe_rgb_to_display;
          }
          else
          {
            pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
            have_lock = FALSE;
            if(buf.color_space == DT_COLORSPACE_NONE)
            {
              fprintf(stderr, "oops, there seems to be a code path not setting the color space of thumbnails!\n");
            }
            else if(buf.color_space != DT_COLORSPACE_DISPLAY)
            {
              fprintf(stderr, "oops, there seems to be a code path setting an unhandled color space of thumbnails (%s)!\n",
                      dt_colorspaces_get_name(buf.color_space, "from file"));
            }
          }
        }

#ifdef _OPENMP
  #pragma omp parallel for schedule(static) default(none) shared(buf, rgbbuf, transform)
#endif
        for(int i = 0; i < buf.height; i++)
        {
          const uint8_t *in = buf.buf + i * buf.width * 4;
          uint8_t *out = rgbbuf + i * buf.width * 4;

          if(transform)
          {
            cmsDoTransform(transform, in, out, buf.width);
          }
          else
          {
            for(int j = 0; j < buf.width; j++, in += 4, out += 4)
            {
              out[0] = in[2];
              out[1] = in[1];
              out[2] = in[0];
            }
          }
        }
        if(have_lock) pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

        const int32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, buf.width);
        surface
          = cairo_image_surface_create_for_data(rgbbuf, CAIRO_FORMAT_RGB24, buf.width, buf.height, stride);
      }

      if(zoom == 1 && !image_only)
      {
        const int32_t tb = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
        scale = fminf((width - 2 * tb) / (float)buf.width, (height - 2 * tb) / (float)buf.height);
      }
      else
        scale = fminf(width * imgwd / (float)buf.width, height * imgwd / (float)buf.height);
    }

    // draw centered and fitted:
    cairo_save(cr);

    if (image_only) // in this case we want to display the picture exactly at (px, py)
      cairo_translate(cr, px, py);
    else
      cairo_translate(cr, width / 2.0, height / 2.0);

    cairo_scale(cr, scale, scale);

    if(buf.buf && surface)
    {
      if (!image_only) cairo_translate(cr, -0.5 * buf.width, -0.5 * buf.height);
      cairo_set_source_surface(cr, surface, 0, 0);
      // set filter no nearest:
      // in skull mode, we want to see big pixels.
      // in 1 iir mode for the right mip, we want to see exactly what the pipe gave us, 1:1 pixel for pixel.
      // in between, filtering just makes stuff go unsharp.
      if((buf.width <= 8 && buf.height <= 8) || fabsf(scale - 1.0f) < 0.01f)
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
      cairo_rectangle(cr, 0, 0, buf.width, buf.height);
      cairo_fill(cr);
      cairo_surface_destroy(surface);

      cairo_rectangle(cr, 0, 0, buf.width, buf.height);
    }

    free(rgbbuf);

    if (image_only)
    {
      cairo_restore(cr);
      cairo_save(cr);
      cairo_new_path(cr);
    }
    else
    {
      // border around image
      cairo_set_source_rgb(cr, bordercol, bordercol, bordercol);
      if(buf.buf && (selected || zoom == 1))
      {
        const float border = zoom == 1 ? DT_PIXEL_APPLY_DPI(16 / scale) : DT_PIXEL_APPLY_DPI(2 / scale);
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1. / scale));
        if(zoom == 1)
        {
          // draw shadow around border
          cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
          cairo_stroke(cr);
          // cairo_new_path(cr);
          cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
          float alpha = 1.0f;
          for(int k = 0; k < 16; k++)
          {
            cairo_rectangle(cr, 0, 0, buf.width, buf.height);
            cairo_new_sub_path(cr);
            cairo_rectangle(cr, -k / scale, -k / scale, buf.width + 2. * k / scale, buf.height + 2. * k / scale);
            cairo_set_source_rgba(cr, 0, 0, 0, alpha);
            alpha *= 0.6f;
            cairo_fill(cr);
          }
        }
        else
        {
          cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
          cairo_new_sub_path(cr);
          cairo_rectangle(cr, -border, -border, buf.width + 2. * border, buf.height + 2. * border);
          cairo_stroke_preserve(cr);
          cairo_set_source_rgb(cr, 1.0 - bordercol, 1.0 - bordercol, 1.0 - bordercol);
          cairo_fill(cr);
        }
      }
      else if(buf.buf)
      {
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5 / scale));
        cairo_stroke(cr);
      }
    }
  }
  cairo_restore(cr);

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  cairo_save(cr);

  const float fscale = DT_PIXEL_APPLY_DPI(fminf(width, height));
  if(imgsel == imgid || full_preview || darktable.gui->show_overlays || zoom == 1)
  {
    if(draw_metadata && width > DECORATION_SIZE_LIMIT)
    {
      // draw mouseover hover effects, set event hook for mouse button down!
      cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5));
      cairo_set_source_rgb(cr, outlinecol, outlinecol, outlinecol);
      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
      float r1, r2;
      if(zoom != 1)
      {
        r1 = 0.05 * width;
        r2 = 0.022 * width;
      }
      else
      {
        r1 = 0.015 * fscale;
        r2 = 0.007 * fscale;
      }

      const gboolean extended_thumb_overlay = dt_conf_get_bool("plugins/lighttable/extended_thumb_overlay");
      float x, y;
      if(zoom != 1)
        y = (extended_thumb_overlay ? 0.93 : 0.9) * height;
      else
        y = .12 * fscale;
      const gboolean image_is_rejected = (img && ((img->flags & 0x7) == 6));

      if(img)
      {
        if (zoom != 1 && (!darktable.gui->show_overlays || imgsel == imgid) && extended_thumb_overlay)
        {
          const double overlay_height = 0.26 * height;
          const int exif_offset = DT_PIXEL_APPLY_DPI(3);
          const int fontsize = 0.18 * overlay_height;
          const double line_offs = 1.15 * fontsize;


          double x0 = DT_PIXEL_APPLY_DPI(1);
          double y0 = height - overlay_height;
          double rect_width = width - DT_PIXEL_APPLY_DPI(2);
          double rect_height = overlay_height - DT_PIXEL_APPLY_DPI(2);
          double radius = DT_PIXEL_APPLY_DPI(5);
          double x1, y1, off, off1;

          x1 = x0 + rect_width;
          y1 = y0 + rect_height;
          off = radius * 0.666;
          off1 = radius - off;
          cairo_save(cr);
          cairo_move_to(cr, x0, y0 + radius);
          cairo_curve_to(cr, x0, y0 + off1, x0 + off1, y0, x0 + radius, y0);
          cairo_line_to(cr, x1 - radius, y0);
          cairo_curve_to(cr, x1 - off1, y0, x1, y0 + off1, x1, y0 + radius);
          cairo_line_to(cr, x1, y1 - radius);
          cairo_curve_to(cr, x1, y1 - off1, x1 - off1, y1, x1 - radius, y1);
          cairo_line_to(cr, x0 + radius, y1);
          cairo_curve_to(cr, x0 + off1, y1, x0, y1 - off1, x0, y1 - radius);
          cairo_close_path(cr);
          cairo_set_source_rgb(cr, bgcol, bgcol, bgcol);
          cairo_fill_preserve(cr);
          cairo_set_line_width(cr, 0.005 * width);
          cairo_set_source_rgb(cr, outlinecol, outlinecol, outlinecol);
          cairo_stroke(cr);

          // some exif data
          PangoLayout *layout;
          PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
          pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
          layout = pango_cairo_create_layout(cr);
          pango_font_description_set_absolute_size(desc, fontsize * PANGO_SCALE);
          pango_layout_set_font_description(layout, desc);
          cairo_set_source_rgb(cr, outlinecol, outlinecol, outlinecol);

          cairo_move_to(cr, x0 + exif_offset, y0 + exif_offset);
          pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_MIDDLE);
          pango_layout_set_width(layout, (int)(PANGO_SCALE * (width - 2 * exif_offset)));
          pango_layout_set_text(layout, img->filename, -1);
          pango_cairo_show_layout(cr, layout);
          char exifline[50];
          cairo_move_to(cr, x0 + exif_offset, y0 + exif_offset + line_offs);
          dt_image_print_exif(img, exifline, sizeof(exifline));
          pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
          pango_layout_set_text(layout, exifline, -1);
          pango_cairo_show_layout(cr, layout);

          pango_font_description_free(desc);
          g_object_unref(layout);
          cairo_restore(cr);
        }

        for(int k = 0; k < 5; k++)
        {
          if(zoom != 1)
            x = (0.26 + k * 0.12) * width;
          else
            x = (.08 + k * 0.04) * fscale;

          if(!image_is_rejected) // if rejected: draw no stars
          {
            dt_view_star(cr, x, y, r1, r2);
            // Only draw hovering effects in stars for the hovered image
            // printf ("Image selected: %d - Image processed: %d\n", imgsel, imgid);
            if((imgsel == imgid || zoom == 1) && ((px - x) * (px - x) + (py - y) * (py - y) < r1 * r1))
            {
              *image_over = DT_VIEW_STAR_1 + k;
              cairo_fill(cr);
            }
            else if((img->flags & 0x7) > k)
            {
              cairo_fill_preserve(cr);
              cairo_set_source_rgb(cr, 1.0 - bordercol, 1.0 - bordercol, 1.0 - bordercol);
              cairo_stroke(cr);
              cairo_set_source_rgb(cr, outlinecol, outlinecol, outlinecol);
            }
            else
              cairo_stroke(cr);
          }
        }
      }

      // Image rejected?
      if(zoom != 1)
        x = 0.08 * width;
      else
        x = .04 * fscale;

      if(image_is_rejected) cairo_set_source_rgb(cr, 1., 0., 0.);

      // Only draw hovering effects in stars for the hovered image
      if((imgsel == imgid || zoom == 1) && ((px - x) * (px - x) + (py - y) * (py - y) < r1 * r1))
      {
        *image_over = DT_VIEW_REJECT; // mouse sensitive
        cairo_new_sub_path(cr);
        cairo_arc(cr, x, y, (r1 + r2) * .5, 0, 2.0f * M_PI);
        cairo_stroke(cr);
      }

      if(image_is_rejected) cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.5));

      // reject cross:
      cairo_move_to(cr, x - r2, y - r2);
      cairo_line_to(cr, x + r2, y + r2);
      cairo_move_to(cr, x + r2, y - r2);
      cairo_line_to(cr, x - r2, y + r2);
      cairo_close_path(cr);
      cairo_stroke(cr);
      cairo_set_source_rgb(cr, outlinecol, outlinecol, outlinecol);
      cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5));

      if(draw_audio)
      {
        if(img && (img->flags & DT_IMAGE_HAS_WAV))
        {
          // align to right
          const float s = (r1 + r2) * .5;
          if(zoom != 1)
          {
            x = width * 0.9 - s * 5;
            y = height * 0.1;
          }
          else
            x = (.04 + 8 * 0.04 - 1.9 * .04) * fscale;
          dt_view_draw_audio(cr, x, y, s);
          // mouse is over the audio icon
          if(fabsf(px - x) <= 1.2 * s && fabsf(py - y) <= 1.2 * s) *image_over = DT_VIEW_AUDIO;
        }
      }

      if(draw_grouping)
      {
        DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.get_grouped);
        DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.get_grouped);
        DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.get_grouped, 1, imgid);
        DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.get_grouped, 2, imgid);

        /* lets check if imgid is in a group */
        if(sqlite3_step(darktable.view_manager->statements.get_grouped) == SQLITE_ROW)
          is_grouped = 1;
        else if(img && darktable.gui->expanded_group_id == img->group_id)
          darktable.gui->expanded_group_id = -1;
      }

      // image part of a group?
      if(is_grouped && darktable.gui && darktable.gui->grouping)
      {
        // draw grouping icon and border if the current group is expanded
        // align to the right, left of altered
        const float s = (r1 + r2) * .6;
        float _x, _y;
        if(zoom != 1)
        {
          _x = width * 0.9 - s * 2.5;
          _y = height * 0.1 - s * .4;
        }
        else
        {
          _x = (.04 + 8 * 0.04 - 1.1 * .04) * fscale;
          _y = y - (.17 * .04) * fscale;
        }
        cairo_save(cr);
        if(img && (imgid != img->group_id)) cairo_set_source_rgb(cr, fontcol, fontcol, fontcol);
        dtgtk_cairo_paint_grouping(cr, _x, _y, s, s, 23, NULL);
        cairo_restore(cr);
        // mouse is over the grouping icon
        if(img && fabs(px - _x - .5 * s) <= .8 * s && fabs(py - _y - .5 * s) <= .8 * s)
          *image_over = DT_VIEW_GROUP;
      }

      // image altered?
      if(draw_history && dt_image_altered(imgid))
      {
        // align to right
        const float s = (r1 + r2) * .5;
        if(zoom != 1)
        {
          x = width * 0.9;
          y = height * 0.1;
        }
        else
          x = (.04 + 8 * 0.04) * fscale;
        dt_view_draw_altered(cr, x, y, s);
        // g_print("px = %d, x = %.4f, py = %d, y = %.4f\n", px, x, py, y);
        if(img && fabsf(px - x) <= 1.2 * s
           && fabsf(py - y) <= 1.2 * s) // mouse hovers over the altered-icon -> history tooltip!
        {
          darktable.gui->center_tooltip = 1;
        }
      }
    }
  }
  cairo_restore(cr);

  // kill all paths, in case img was not loaded yet, or is blocked:
  cairo_new_path(cr);

  if (draw_colorlabels)
  {
    // TODO: make mouse sensitive, just as stars!
    // TODO: cache in image struct!

    // TODO: there is a branch that sets the bg == colorlabel
    //       this might help if zoom > 15
    if(width > DECORATION_SIZE_LIMIT)
    {
      // color labels:
      const float x[] = {0.84, 0.92, 0.88, 0.84, 0.92};
      const float y[] = {0.84, 0.84, 0.88, 0.92, 0.92};
      const float x_zoom[] = {0.27, 0.30, 0.285, 0.27, 0.30};
      const float y_zoom[] = {0.095, 0.095, 0.11, 0.125, 0.125};
      const int max_col = sizeof(x) / sizeof(x[0]);
      const float r = zoom == 1 ? 0.01 * fscale : 0.03 * width;

      gboolean colorlabel_painted = FALSE;
      gboolean painted_col[] = {FALSE, FALSE, FALSE, FALSE, FALSE};

      /* clear and reset prepared statement */
      DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.get_color);
      DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.get_color);

      /* setup statement and iterate rows */
      DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.get_color, 1, imgid);
      while(sqlite3_step(darktable.view_manager->statements.get_color) == SQLITE_ROW)
      {
        cairo_save(cr);
        const int col = sqlite3_column_int(darktable.view_manager->statements.get_color, 0);
        if (col < max_col)
        {
          // see src/dtgtk/paint.c
          if (zoom != 1)
            dtgtk_cairo_paint_label(cr, x[col]  * width, y[col] * height, r * 2, r * 2, col, NULL);
          else
            dtgtk_cairo_paint_label(cr, x_zoom[col]  * fscale, y_zoom[col] * fscale, r * 2, r * 2, col, NULL);
          colorlabel_painted = colorlabel_painted || TRUE;
          painted_col[col] = TRUE;
        }
        cairo_restore(cr);
      }
      if (colorlabel_painted)
      {
        const int dont_fill_col = 7;
        for(int i = 0; i < max_col; i++)
        {
          if (!painted_col[i])
          {
            cairo_save(cr);
            if (zoom != 1)
              dtgtk_cairo_paint_label(cr, x[i]  * width, y[i] * height, r * 2, r * 2, dont_fill_col, NULL);
            else
              dtgtk_cairo_paint_label(cr, x_zoom[i]  * fscale, y_zoom[i] * fscale, r * 2, r * 2, dont_fill_col, NULL);
            cairo_restore(cr);
          }
        }
      }
    }
  }

  if (draw_local_copy)
  {
    if(img && width > DECORATION_SIZE_LIMIT)
    {
      const gboolean has_local_copy = (img && (img->flags & DT_IMAGE_LOCAL_COPY));

      if (has_local_copy)
      {
        cairo_save(cr);

        if (zoom != 1)
        {
          double x0 = DT_PIXEL_APPLY_DPI(1), y0 = DT_PIXEL_APPLY_DPI(1), rect_width = width - DT_PIXEL_APPLY_DPI(2),
                radius = DT_PIXEL_APPLY_DPI(5);
          double x1, off, off1;

          x1 = x0 + rect_width;
          off = radius * 0.666;
          off1 = radius - off;

          cairo_move_to(cr, x1 - width * 0.08, y0);
          cairo_line_to(cr, x1 - radius, y0);
          cairo_curve_to(cr, x1 - off1, y0, x1, y0 + off1, x1, y0 + radius);
          cairo_line_to(cr, x1, y0 + height * 0.08);
          cairo_close_path(cr);
          cairo_set_source_rgb(cr, 1, 1, 1);
          cairo_fill_preserve(cr);
          cairo_set_line_width(cr, 0.005 * width);
          cairo_set_source_rgb(cr, outlinecol, outlinecol, outlinecol);
          cairo_stroke(cr);
        }
        else
        {
          const float x_zoom = 0.325;
          const float y_zoom = 0.112;
          const float edge_length = 0.016 * fscale;

          cairo_rectangle(cr, x_zoom * fscale, y_zoom * fscale, edge_length, edge_length);
          cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
          cairo_set_line_width(cr, 0.002 * fscale);
          cairo_stroke(cr);

          cairo_move_to(cr, x_zoom * fscale + edge_length * 0.1, y_zoom * fscale);
          cairo_line_to(cr, x_zoom * fscale + edge_length, y_zoom * fscale);
          cairo_line_to(cr, x_zoom * fscale + edge_length, y_zoom * fscale + edge_length * 0.9);
          cairo_close_path(cr);
          cairo_set_source_rgb(cr, 1, 1, 1);
          cairo_fill_preserve(cr);
          cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
          cairo_stroke(cr);
        }
        cairo_restore(cr);
      }
    }
  }

  if(draw_metadata && img && (zoom == 1))
  {
    // some exif data
    PangoLayout *layout;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    layout = pango_cairo_create_layout(cr);
    const int fontsize = 0.025 * fscale;
    pango_font_description_set_absolute_size(desc, fontsize * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0));
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);

    cairo_move_to(cr, .02 * fscale, .04 * fscale - fontsize);
    pango_layout_set_text(layout, img->filename, -1);
    pango_cairo_layout_path(cr, layout);
    char exifline[50];
    cairo_move_to(cr, .02 * fscale, .08 * fscale - fontsize);
    dt_image_print_exif(img, exifline, sizeof(exifline));
    pango_layout_set_text(layout, exifline, -1);
    pango_cairo_layout_path(cr, layout);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgb(cr, .7, .7, .7);
    cairo_fill(cr);
    pango_font_description_free(desc);
    g_object_unref(layout);

  }

  // draw custom metadata from accompanying text file:
  if(draw_metadata && img && (img->flags & DT_IMAGE_HAS_TXT) && dt_conf_get_bool("plugins/lighttable/draw_custom_metadata")
     && (zoom == 1))
  {
    char *path = dt_image_get_text_path(img->id);
    if(path)
    {
      FILE *f = g_fopen(path, "rb");
      if(f)
      {
        char line[2048];
        PangoLayout *layout;
        PangoFontDescription *desc = pango_font_description_from_string("monospace bold");
        layout = pango_cairo_create_layout(cr);
        const float fontsize = 0.015 * fscale;
        pango_font_description_set_absolute_size(desc, fontsize * PANGO_SCALE);
        pango_layout_set_font_description(layout, desc);
        // cairo_set_operator(cr, CAIRO_OPERATOR_XOR);
        cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0));
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        int k = 0;
        while(!feof(f))
        {
          gchar *line_pattern = g_strdup_printf("%%%zu[^\n]", sizeof(line) - 1);
          const int read = fscanf(f, line_pattern, line);
          g_free(line_pattern);
          if(read != 1) break;
          fgetc(f); // munch \n

          cairo_move_to(cr, .02 * fscale, .20 * fscale + .017 * fscale * k - fontsize);
          cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
          pango_layout_set_text(layout, line, -1);
          pango_cairo_layout_path(cr, layout);
          cairo_stroke_preserve(cr);
          cairo_set_source_rgb(cr, .7, .7, .7);
          cairo_fill(cr);
          k++;
        }
        fclose(f);
        pango_font_description_free(desc);
        g_object_unref(layout);

      }
      g_free(path);
    }
  }

  cairo_restore(cr);
  // if(zoom == 1) cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);

  const double end = dt_get_wtime();
  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_LIGHTTABLE, "[lighttable] image expose took %0.04f sec\n", end - start);
  return missing;
}

void
dt_view_image_only_expose(
  uint32_t imgid,
  cairo_t *cr,
  int32_t width,
  int32_t height,
  int32_t offsetx,
  int32_t offsety)
{
  dt_view_image_over_t image_over;
  dt_view_image_expose(&image_over, imgid, cr, width, height, 1, offsetx, offsety, TRUE, TRUE);
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
 * \brief Reset filter
 */
void dt_view_filter_reset(const dt_view_manager_t *vm, gboolean smart_filter)
{
  if(vm->proxy.filter.module && vm->proxy.filter.reset_filter)
    vm->proxy.filter.reset_filter(vm->proxy.filter.module, smart_filter);
}

void dt_view_filmstrip_scroll_relative(const int diff, int offset)
{
  const gchar *qin = dt_collection_get_query(darktable.collection);
  if(qin)
  {
    sqlite3_stmt *stmt;

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), qin, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, offset + diff);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, 1);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int imgid = sqlite3_column_int(stmt, 0);

      if(!darktable.develop->image_loading)
      {
        dt_view_filmstrip_scroll_to_image(darktable.view_manager, imgid, TRUE);
      }
    }
    sqlite3_finalize(stmt);
  }
}

void dt_view_filmstrip_scroll_to_image(dt_view_manager_t *vm, const int imgid, gboolean activate)
{
  // g_return_if_fail(vm->proxy.filmstrip.module!=NULL); // This can happen here for debugging
  // g_return_if_fail(vm->proxy.filmstrip.scroll_to_image!=NULL);

  if(vm->proxy.filmstrip.module && vm->proxy.filmstrip.scroll_to_image)
    vm->proxy.filmstrip.scroll_to_image(vm->proxy.filmstrip.module, imgid, activate);
}

int32_t dt_view_filmstrip_get_activated_imgid(dt_view_manager_t *vm)
{
  // g_return_val_if_fail(vm->proxy.filmstrip.module!=NULL, 0); // This can happen here for debugging
  // g_return_val_if_fail(vm->proxy.filmstrip.activated_image!=NULL, 0);

  if(vm->proxy.filmstrip.module && vm->proxy.filmstrip.activated_image)
    return vm->proxy.filmstrip.activated_image(vm->proxy.filmstrip.module);

  return 0;
}

void dt_view_filmstrip_set_active_image(dt_view_manager_t *vm, int iid)
{
  /* First off clear all selected images... */
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);

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
  const gchar *qin = dt_collection_get_query(darktable.collection);
  if(!qin) return;

  int offset = 0;
  if(qin)
  {
    int imgid = -1;
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1, &stmt,
                                NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW) imgid = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    offset = dt_collection_image_offset(imgid);
  }

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), qin, -1, &stmt, NULL);
  // only get one more image:
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, offset + 1);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, offset + 2);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const uint32_t prefetchid = sqlite3_column_int(stmt, 0);
    // dt_control_log("prefetching image %u", prefetchid);
    dt_mipmap_cache_get(darktable.mipmap_cache, NULL, prefetchid, DT_MIPMAP_FULL, DT_MIPMAP_PREFETCH, 'r');
  }
  sqlite3_finalize(stmt);
}

void dt_view_manager_view_toolbox_add(dt_view_manager_t *vm, GtkWidget *tool, dt_view_type_flags_t views)
{
  if(vm->proxy.view_toolbox.module) vm->proxy.view_toolbox.add(vm->proxy.view_toolbox.module, tool, views);
}

void dt_view_manager_module_toolbox_add(dt_view_manager_t *vm, GtkWidget *tool, dt_view_type_flags_t views)
{
  if(vm->proxy.module_toolbox.module) vm->proxy.module_toolbox.add(vm->proxy.module_toolbox.module, tool, views);
}

void dt_view_lighttable_set_zoom(dt_view_manager_t *vm, gint zoom)
{
  if(vm->proxy.lighttable.module) vm->proxy.lighttable.set_zoom(vm->proxy.lighttable.module, zoom);
}

void dt_view_lighttable_set_position(dt_view_manager_t *vm, uint32_t pos)
{
  if(vm->proxy.lighttable.view) vm->proxy.lighttable.set_position(vm->proxy.lighttable.view, pos);

  // ugh. but will go away once module guis are persistent between views:
  dt_conf_set_int("plugins/lighttable/recentcollect/pos0", pos);
}

uint32_t dt_view_lighttable_get_position(dt_view_manager_t *vm)
{
  if(vm->proxy.lighttable.view) return vm->proxy.lighttable.get_position(vm->proxy.lighttable.view);
  return 0;
}

void dt_view_collection_update(const dt_view_manager_t *vm)
{
  if(vm->proxy.module_collect.module) vm->proxy.module_collect.update(vm->proxy.module_collect.module);
}


int32_t dt_view_tethering_get_selected_imgid(const dt_view_manager_t *vm)
{
  if(vm->proxy.tethering.view) return vm->proxy.tethering.get_selected_imgid(vm->proxy.tethering.view);

  return -1;
}

void dt_view_tethering_set_job_code(const dt_view_manager_t *vm, const char *name)
{
  if(vm->proxy.tethering.view) vm->proxy.tethering.set_job_code(vm->proxy.tethering.view, name);
}

const char *dt_view_tethering_get_job_code(const dt_view_manager_t *vm)
{
  if(vm->proxy.tethering.view) return vm->proxy.tethering.get_job_code(vm->proxy.tethering.view);
  return NULL;
}

#ifdef HAVE_MAP
void dt_view_map_center_on_location(const dt_view_manager_t *vm, gdouble lon, gdouble lat, gdouble zoom)
{
  if(vm->proxy.map.view) vm->proxy.map.center_on_location(vm->proxy.map.view, lon, lat, zoom);
}

void dt_view_map_center_on_bbox(const dt_view_manager_t *vm, gdouble lon1, gdouble lat1, gdouble lon2, gdouble lat2)
{
  if(vm->proxy.map.view) vm->proxy.map.center_on_bbox(vm->proxy.map.view, lon1, lat1, lon2, lat2);
}

void dt_view_map_show_osd(const dt_view_manager_t *vm, gboolean enabled)
{
  if(vm->proxy.map.view) vm->proxy.map.show_osd(vm->proxy.map.view, enabled);
}

void dt_view_map_set_map_source(const dt_view_manager_t *vm, OsmGpsMapSource_t map_source)
{
  if(vm->proxy.map.view) vm->proxy.map.set_map_source(vm->proxy.map.view, map_source);
}

GObject *dt_view_map_add_marker(const dt_view_manager_t *vm, dt_geo_map_display_t type, GList *points)
{
  if(vm->proxy.map.view) return vm->proxy.map.add_marker(vm->proxy.map.view, type, points);
  return NULL;
}

gboolean dt_view_map_remove_marker(const dt_view_manager_t *vm, dt_geo_map_display_t type, GObject *marker)
{
  if(vm->proxy.map.view) return vm->proxy.map.remove_marker(vm->proxy.map.view, type, marker);
  return FALSE;
}
#endif

#ifdef HAVE_PRINT
void dt_view_print_settings(const dt_view_manager_t *vm, dt_print_info_t *pinfo)
{
  if (vm->proxy.print.view)
    vm->proxy.print.print_settings(vm->proxy.print.view, pinfo);
}
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
