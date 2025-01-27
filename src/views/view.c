/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.

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

#include "common/extra_optimizations.h"

#include "views/view.h"
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/focus_peaking.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/module.h"
#include "common/selection.h"
#include "common/undo.h"
#include "common/usermanual_url.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/button.h"
#include "dtgtk/expander.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
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

static void dt_view_manager_load_modules(dt_view_manager_t *vm);
static int dt_view_load_module(void *v,
                               const char *libname,
                               const char *module_name);
static void dt_view_unload_module(dt_view_t *view);

void dt_view_manager_init(dt_view_manager_t *vm)
{
  /* prepare statements */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid FROM main.selected_images "
                              "WHERE imgid = ?1", -1, &vm->statements.is_selected, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.selected_images WHERE imgid = ?1",
                              -1, &vm->statements.delete_from_selected, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT OR IGNORE INTO main.selected_images (imgid)"
                              " VALUES (?1)", -1,
                              &vm->statements.make_selected, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT num FROM main.history WHERE imgid = ?1", -1,
                              &vm->statements.have_history, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT color FROM main.color_labels WHERE imgid=?1",
                              -1, &vm->statements.get_color, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id"
                              " FROM main.images"
                              " WHERE group_id = (SELECT group_id"
                              "                   FROM main.images"
                              "                   WHERE id=?1)"
                              "   AND id != ?2",
                              -1, &vm->statements.get_grouped, NULL);

  dt_view_manager_load_modules(vm);

  vm->current_view = NULL;
  vm->audio.audio_player_id = -1;
}

void dt_view_manager_gui_init(dt_view_manager_t *vm)
{
  for(GList *iter = vm->views; iter; iter = g_list_next(iter))
  {
    dt_view_t *view = iter->data;
    if(view->gui_init) view->gui_init(view);
  }
}

void dt_view_manager_cleanup(dt_view_manager_t *vm)
{
  for(GList *iter = vm->views;
      iter;
      iter = g_list_next(iter))
    dt_view_unload_module((dt_view_t *)iter->data);

  g_list_free_full(vm->views, free);
  vm->views = NULL;
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
    if(!strcmp(av->module_name, view_order[i]))
      apos = i;
    if(!strcmp(bv->module_name, view_order[i]))
      bpos = i;
  }

  // order will be zero iff apos == bpos which can only happen when
  // both views are not in view_order
  const int order = apos - bpos;
  return order ? order : strcmp(aname, bname);
}

static void dt_view_manager_load_modules(dt_view_manager_t *vm)
{
  vm->views = dt_module_load_modules("/views", sizeof(dt_view_t),
                                     dt_view_load_module, NULL, sort_views);
}

/* default flags for view which does not implement the flags() function */
static uint32_t default_flags()
{
  return 0;
}

/** load a view module */
static int dt_view_load_module(void *v,
                               const char *libname,
                               const char *module_name)
{
  dt_view_t *module = (dt_view_t *)v;
  g_strlcpy(module->module_name, module_name, sizeof(module->module_name));

#define INCLUDE_API_FROM_MODULE_LOAD "view_load_module"
#include "views/view_api.h"

  module->data = NULL;
  module->vscroll_size = module->vscroll_viewport_size = 1.0;
  module->hscroll_size = module->hscroll_viewport_size = 1.0;
  module->vscroll_pos = module->hscroll_pos = 0.0;
  module->height = module->width = 100; // set to non-insane defaults
                                        // before first
                                        // expose/configure.

#ifdef USE_LUA
  dt_lua_register_view(darktable.lua_state.state, module);
#endif

  if(module->init) module->init(module);

  if(darktable.gui)
  {
    module->actions = (dt_action_t){ DT_ACTION_TYPE_VIEW,
                                     module->module_name,
                                     module->name(module) };
    dt_action_insert_sorted(&darktable.control->actions_views, &module->actions);
  }

  return 0;
}

/** unload, cleanup */
static void dt_view_unload_module(dt_view_t *view)
{
  if(view->cleanup)
    view->cleanup(view);

  if(view->module)
    g_module_close(view->module);
}

void dt_vm_remove_child(GtkWidget *widget, gpointer data)
{
  gtk_container_remove(GTK_CONTAINER(data), widget);
}

gboolean dt_view_manager_switch(dt_view_manager_t *vm,
                                const char *view_name)
{
  gboolean switching_to_none = *view_name == '\0';
  dt_view_t *new_view = NULL;

  if(!switching_to_none)
  {
    for(GList *iter = vm->views; iter; iter = g_list_next(iter))
    {
      dt_view_t *v = iter->data;
      if(!g_ascii_strcasecmp(v->module_name, view_name))
      {
        new_view = v;
        break;
      }
    }
    if(!new_view)
      return TRUE; // the requested view doesn't exist
  }

  return dt_view_manager_switch_by_view(vm, new_view);
}

gboolean dt_view_manager_switch_by_view(dt_view_manager_t *vm,
                                        const dt_view_t *nv)
{
  dt_view_t *old_view = vm->current_view;
  dt_view_t *new_view = (dt_view_t *)nv; // views belong to us, we can de-const them :-)

  // possibly avoid switch for as we are gimping
  if(old_view
      && new_view
      && dt_check_gimpmode("file")
      && dt_view_get_current() == DT_VIEW_DARKROOM)
    return FALSE;

  // reset the cursor to the default one
  dt_control_change_cursor(GDK_LEFT_PTR);

  dt_set_backthumb_time(0.0);

  // destroy old module list

  /* clear the undo list, for now we do this unconditionally. At some
     point we will probably want to clear only part of the undo
     list. This should probably done with a view proxy routine
     returning the type of undo to remove. */
  dt_undo_clear(darktable.undo, DT_UNDO_ALL);

  /* Special case when entering nothing (just before leaving dt) */
  if(!new_view)
  {
    if(old_view)
    {
      /* leave the current view*/
      if(old_view->leave) old_view->leave(old_view);

      /* iterator plugins and cleanup plugins in current view */
      for(GList *iter = darktable.lib->plugins;
          iter;
          iter = g_list_next(iter))
      {
        dt_lib_module_t *plugin = iter->data;

        /* does this module belong to current view ?*/
        if(dt_lib_is_visible_in_view(plugin, old_view))
        {
          if(plugin->view_leave) plugin->view_leave(plugin, old_view, NULL);
          plugin->gui_cleanup(plugin);
          plugin->data = NULL;
          plugin->widget = NULL;
        }
      }
    }

    /* remove all widgets in all containers */
    for(int l = 0; l < DT_UI_CONTAINER_SIZE; l++)
      dt_ui_container_destroy_children(darktable.gui->ui, l);
    vm->current_view = NULL;

    /* remove sticky accels window */
    if(vm->accels_window.window)
      dt_view_accels_hide(vm);
    return FALSE;
  }

  // invariant: new_view != NULL after this point
  assert(new_view != NULL);

  if(new_view->try_enter)
  {
    const gboolean error = new_view->try_enter(new_view);
    if(error)
    {
      DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_VIEWMANAGER_VIEW_CANNOT_CHANGE,
                              old_view, new_view);
      return error;
    }
  }

  /* cleanup current view before initialization of new  */
  if(old_view)
  {
    /* leave current view */
    if(new_view != old_view && old_view->leave)
      old_view->leave(old_view);

    /* iterator plugins and cleanup plugins in current view */
    for(GList *iter = darktable.lib->plugins;
        iter;
        iter = g_list_next(iter))
    {
      dt_lib_module_t *plugin = iter->data;

      if(new_view == old_view && !plugin->expandable(plugin)) continue;

      /* does this module belong to current view ?*/
      GtkWidget *ppw = plugin->expander ?: plugin->widget;
      if(ppw && gtk_widget_get_ancestor(ppw, GTK_TYPE_WINDOW))
      {
        if(plugin->view_leave)
          plugin->view_leave(plugin, old_view, new_view);

        /*
          When expanders get destroyed, they destroy the child
          so remove the child before that
          */
        if(plugin->widget)
          gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(plugin->widget)), plugin->widget);
        if(plugin->expander)
          gtk_widget_destroy(plugin->expander);
      }
      plugin->expander = NULL;
    }
  }

  /* change current view to the new view */
  vm->current_view = new_view;

  dt_view_type_flags_t view_type = new_view->view(new_view);

  if(new_view != old_view) // implement preference toggle only on view change
    dt_ui_container_swap_left_right(darktable.gui->ui, view_type == DT_VIEW_DARKROOM
                                    && dt_conf_get_bool("plugins/darkroom/panel_swap"));

  /* restore visible stat of panels for the new view */
  dt_ui_restore_panels(darktable.gui->ui);

  /* lets add plugins related to new view into panels.  this has to be
   * done in reverse order to have the lowest position at the
   * bottom! */

  // adjust order per view in case user made changes
  darktable.lib->plugins = g_list_sort(darktable.lib->plugins, dt_lib_sort_plugins);

  for(GList *iter = g_list_last(darktable.lib->plugins);
      iter;
      iter = g_list_previous(iter))
  {
    dt_lib_module_t *plugin = iter->data;
    GtkWidget *w = plugin->widget;

    if(plugin->expandable(plugin))
    {
      if(!dt_lib_is_visible_in_view(plugin, new_view))
        continue;

      w = dt_lib_gui_get_expander(plugin);

      /* set expanded if last mode was that */
      char var[1024];
      snprintf(var, sizeof(var), "plugins/%s/%s/expanded",
               new_view->module_name, plugin->plugin_name);
      gboolean expanded = dt_conf_get_bool(var);
      dt_lib_gui_set_expanded(plugin, expanded);
      dt_lib_set_visible(plugin, TRUE);
    }
    else if(new_view != old_view && plugin->views(plugin) & view_type)
    {
      dt_lib_gui_get_expander(plugin); // connect modulegroups presets button

      if(dt_lib_is_visible(plugin))
        gtk_widget_show_all(plugin->widget);
      else
        gtk_widget_hide(plugin->widget);
    }
    else
      continue;

    if(plugin->view_enter)
      plugin->view_enter(plugin, old_view, new_view);

    dt_gui_add_help_link(w, plugin->plugin_name);
    // some plugins help links depend on the view
    if(!strcmp(plugin->plugin_name,"module_toolbox")
      || !strcmp(plugin->plugin_name,"view_toolbox"))
    {
      if(view_type == DT_VIEW_LIGHTTABLE)
                       dt_gui_add_help_link(w, "lighttable_mode");
      if(view_type == DT_VIEW_DARKROOM)
        dt_gui_add_help_link(w, "darkroom_bottom_panel");
    }

    /* add module to its container */
    dt_ui_container_add_widget(darktable.gui->ui, dt_lib_get_container(plugin), w);
  }

  darktable.lib->gui_module = NULL;

  /* enter view. crucially, do this before initing the plugins below,
      as e.g. modulegroups requires the dr stuff to be inited. */
  if(new_view != old_view && new_view->enter)
    new_view->enter(new_view);

  /* update the scrollbars */
  dt_ui_update_scrollbars(darktable.gui->ui);

  dt_shortcuts_select_view(view_type);

  /* update sticky accels window */
  if(vm->accels_window.window && vm->accels_window.sticky) dt_view_accels_refresh(vm);

  /* raise view changed signal */
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED, old_view, new_view);

  // update log visibility
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_CONTROL_LOG_REDRAW);

  // update toast visibility
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_CONTROL_TOAST_REDRAW);
  return FALSE;
}

const char *dt_view_manager_name(dt_view_manager_t *vm)
{
  if(!vm->current_view)
    return "";
  if(vm->current_view->name)
    return vm->current_view->name(vm->current_view);
  else
    return vm->current_view->module_name;
}

void dt_view_manager_expose(dt_view_manager_t *vm,
                            cairo_t *cr,
                            const int32_t width,
                            const int32_t height,
                            const int32_t pointerx,
                            const int32_t pointery)
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
    vm->current_view->expose(vm->current_view, cr,
                             vm->current_view->width,
                             vm->current_view->height, px, py);

    cairo_restore(cr);
    /* expose plugins */
    for(const GList *plugins = g_list_last(darktable.lib->plugins);
        plugins;
        plugins = g_list_previous(plugins))
    {
      dt_lib_module_t *plugin = plugins->data;

      /* does this module belong to current view ?*/
      if(plugin->gui_post_expose
         && dt_lib_is_visible_in_view(plugin, vm->current_view))
        plugin->gui_post_expose(plugin, cr,
                                vm->current_view->width,
                                vm->current_view->height, px, py);
    }
  }
}

void dt_view_manager_reset(dt_view_manager_t *vm)
{
  if(!vm->current_view)
    return;
  if(vm->current_view->reset)
    vm->current_view->reset(vm->current_view);
}

void dt_view_manager_mouse_leave(dt_view_manager_t *vm)
{
  if(!vm->current_view) return;
  dt_view_t *v = vm->current_view;

  /* lets check if any plugins want to handle mouse move */
  gboolean handled = FALSE;
  for(const GList *plugins = g_list_last(darktable.lib->plugins);
      plugins;
      plugins = g_list_previous(plugins))
  {
    dt_lib_module_t *plugin = plugins->data;

    /* does this module belong to current view ?*/
    if(plugin->mouse_leave && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->mouse_leave(plugin))
        handled = TRUE;
  }

  /* if not handled by any plugin let pass to view handler*/
  if(!handled && v->mouse_leave)
    v->mouse_leave(v);
}

void dt_view_manager_mouse_enter(dt_view_manager_t *vm)
{
  if(!vm->current_view)
    return;
  if(vm->current_view->mouse_enter)
    vm->current_view->mouse_enter(vm->current_view);
}

void dt_view_manager_mouse_moved(dt_view_manager_t *vm,
                                 const double x,
                                 const double y,
                                 const double pressure,
                                 const int which)
{
  if(!vm->current_view) return;
  dt_view_t *v = vm->current_view;

  /* lets check if any plugins want to handle mouse move */
  gboolean handled = FALSE;
  for(const GList *plugins = g_list_last(darktable.lib->plugins);
      plugins;
      plugins = g_list_previous(plugins))
  {
    dt_lib_module_t *plugin = plugins->data;

    /* does this module belong to current view ?*/
    if(plugin->mouse_moved && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->mouse_moved(plugin, x, y, pressure, which))
        handled = TRUE;
  }

  /* if not handled by any plugin let pass to view handler*/
  if(!handled && v->mouse_moved)
    v->mouse_moved(v, x, y, pressure, which);
}

int dt_view_manager_button_released(dt_view_manager_t *vm,
                                    const double x,
                                    const double y,
                                    const int which,
                                    const uint32_t state)
{
  if(!vm->current_view)
    return 0;
  dt_view_t *v = vm->current_view;

  /* lets check if any plugins want to handle button press */
  gboolean handled = FALSE;
  for(const GList *plugins = g_list_last(darktable.lib->plugins);
      plugins;
      plugins = g_list_previous(plugins))
  {
    dt_lib_module_t *plugin = plugins->data;

    /* does this module belong to current view ?*/
    if(plugin->button_released && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->button_released(plugin, x, y, which, state))
        handled = TRUE;
  }

  if(handled)
    return 1;
  /* if not handled by any plugin let pass to view handler*/
  else if(v->button_released)
    v->button_released(v, x, y, which, state);

  return 0;
}

int dt_view_manager_button_pressed(dt_view_manager_t *vm,
                                   const double x,
                                   const double y,
                                   const double pressure,
                                   const int which,
                                   const int type,
                                   const uint32_t state)
{
  if(!vm->current_view)
    return 0;
  dt_view_t *v = vm->current_view;

  /* lets check if any plugins want to handle button press */
  gboolean handled = FALSE;
  for(const GList *plugins = g_list_last(darktable.lib->plugins);
      plugins && !handled;
      plugins = g_list_previous(plugins))
  {
    dt_lib_module_t *plugin = plugins->data;

    /* does this module belong to current view ?*/
    if(plugin->button_pressed && dt_lib_is_visible_in_view(plugin, v))
      if(plugin->button_pressed(plugin, x, y, pressure, which, type, state))
        handled = TRUE;
  }

  if(handled)
    return 1;
  /* if not handled by any plugin let pass to view handler*/
  else if(v->button_pressed)
    return v->button_pressed(v, x, y, pressure, which, type, state);

  return 0;
}

void dt_view_manager_configure(dt_view_manager_t *vm,
                               const int width,
                               const int height)
{
  for(GList *iter = vm->views; iter; iter = g_list_next(iter))
  {
    // this is necessary for all
    dt_view_t *v = iter->data;
    v->width = width;
    v->height = height;
    if(v->configure)
      v->configure(v, width, height);
  }
}

void dt_view_manager_scrolled(dt_view_manager_t *vm,
                              const double x,
                              const double y,
                              const int up,
                              const int state)
{
  if(!vm->current_view)
    return;
  if(vm->current_view->scrolled)
    vm->current_view->scrolled(vm->current_view, x, y, up, state);
}

void dt_view_manager_scrollbar_changed(dt_view_manager_t *vm,
                                       const double x,
                                       const double y)
{
  if(!vm->current_view)
    return;
  if(vm->current_view->scrollbar_changed)
    vm->current_view->scrollbar_changed(vm->current_view, x, y);
}

void dt_view_set_scrollbar(dt_view_t *view,
                           const float hpos,
                           const float hlower,
                           const float hsize,
                           const float hwinsize,
                           const float vpos,
                           const float vlower,
                           const float vsize,
                           const float vwinsize)
{
  view->vscroll_pos = vpos;
  view->vscroll_lower = vlower;
  view->vscroll_size = vsize;
  view->vscroll_viewport_size = vwinsize;
  view->hscroll_pos = hpos;
  view->hscroll_lower = hlower;
  view->hscroll_size = hsize;
  view->hscroll_viewport_size = hwinsize;

  dt_gui_widgets_t *widgets = &darktable.gui->widgets;
  gtk_widget_queue_draw(widgets->left_border);
  gtk_widget_queue_draw(widgets->right_border);
  gtk_widget_queue_draw(widgets->bottom_border);
  gtk_widget_queue_draw(widgets->top_border);

  dt_ui_update_scrollbars(darktable.gui->ui);
}

dt_view_surface_value_t dt_view_image_get_surface(const dt_imgid_t imgid,
                                                  const int32_t width,
                                                  const int32_t height,
                                                  cairo_surface_t **surface,
                                                  const gboolean quality)
{
  const double tt = dt_get_debug_wtime();

  dt_view_surface_value_t ret = DT_VIEW_SURFACE_KO;
  // if surface not null, clean it up
  if(*surface
     && cairo_surface_get_reference_count(*surface) > 0)
    cairo_surface_destroy(*surface);

  *surface = NULL;

  // get mipmap cache image
  dt_mipmap_cache_t *cache = darktable.mipmap_cache;
  const int32_t mipwidth = width * darktable.gui->ppd;
  const int32_t mipheight = height * darktable.gui->ppd;
  dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(cache, mipwidth, mipheight);

  // if needed, we load the mimap buffer
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(cache, &buf, imgid, mip, DT_MIPMAP_BEST_EFFORT, 'r');

  const int32_t buf_wd = buf.width;
  const int32_t buf_ht = buf.height;

  dt_print(DT_DEBUG_LIGHTTABLE,
      "dt_view_image_get_surface  id %i, dots %ix%i -> mip %ix%i, found %ix%i",
      imgid, mipwidth, mipheight, cache->max_width[mip], cache->max_height[mip], buf_wd, buf_ht);

  // no image is available at the moment as we didn't get buffer data
  if(!buf.buf)
  {
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    return DT_VIEW_SURFACE_KO;
  }

  // so we create a new image surface to return
  float scale = fminf(width / (float)buf_wd,
                      height / (float)buf_ht) * darktable.gui->ppd_thb;
  const int32_t img_width = roundf(buf_wd * scale);
  const int32_t img_height = roundf(buf_ht * scale);
  // due to the forced rounding above, we need to recompute scaling
  scale = fmaxf(img_width / (float)buf_wd, img_height / (float)buf_ht);
  *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, img_width, img_height);

  // we transfer cached image on a cairo_surface (with colorspace transform if needed)
  cairo_surface_t *tmp_surface = NULL;
  uint8_t *rgbbuf = calloc((size_t)buf_wd * buf_ht * 4, sizeof(uint8_t));
  if(rgbbuf)
  {
    gboolean have_lock = FALSE;
    cmsHTRANSFORM transform = NULL;

    if(dt_conf_get_bool("cache_color_managed"))
    {
      pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
      have_lock = TRUE;

      // we only color manage when a thumbnail is sRGB or
      // AdobeRGB. everything else just gets dumped to the screen
      if(buf.color_space == DT_COLORSPACE_SRGB
         && darktable.color_profiles->transform_srgb_to_display)
      {
        transform = darktable.color_profiles->transform_srgb_to_display;
      }
      else if(buf.color_space == DT_COLORSPACE_ADOBERGB
              && darktable.color_profiles->transform_adobe_rgb_to_display)
      {
        transform = darktable.color_profiles->transform_adobe_rgb_to_display;
      }
      else
      {
        pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
        have_lock = FALSE;
        if(buf.color_space == DT_COLORSPACE_NONE)
        {
          dt_print(DT_DEBUG_ALWAYS,
                  "oops, there seems to be a code path not setting the"
                  " color space of thumbnails!\n");
        }
        else if(buf.color_space != DT_COLORSPACE_DISPLAY
                && buf.color_space != DT_COLORSPACE_DISPLAY2)
        {
          dt_print(DT_DEBUG_ALWAYS,
                  "oops, there seems to be a code path setting an"
                  " unhandled color space of thumbnails (%s)!",
                  dt_colorspaces_get_name(buf.color_space, "from file"));
        }
      }
    }

    DT_OMP_FOR()
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
    if(have_lock)
      pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);

    const int32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, buf_wd);
    tmp_surface = cairo_image_surface_create_for_data(rgbbuf,
                                                      CAIRO_FORMAT_RGB24,
                                                      buf_wd, buf_ht, stride);
  }

  // draw the image scaled:
  if(tmp_surface)
  {
    cairo_t *cr = cairo_create(*surface);
    cairo_scale(cr, scale, scale);

    cairo_set_source_surface(cr, tmp_surface, 0, 0);
    // set filter no nearest: in skull/error mode, we want to see big
    // pixels.  in 1 iir mode for the right mip, we want to see
    // exactly what the pipe gave us, 1:1 pixel for pixel.  in
    // between, filtering just makes stuff go unsharp.
    if((buf_wd <= 30 && buf_ht <= 30)
       || fabsf(scale - 1.0f) < 0.01f)
      cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    else if(mip != buf.size)
      cairo_pattern_set_filter(cairo_get_source(cr),
                               CAIRO_FILTER_FAST); // not the right
                                                   // size, so we
                                                   // scale as fast a
                                                   // possible
    else
      cairo_pattern_set_filter(cairo_get_source(cr),
                               ((darktable.gui->filter_image == CAIRO_FILTER_FAST)
                                && quality)
                               ? CAIRO_FILTER_GOOD
                               : darktable.gui->filter_image);

    cairo_paint(cr);
    /* from focus_peaking.h
       static inline void dt_focuspeaking(cairo_t *cr, int width, int height,
                                       uint8_t *const restrict image,
                                       const int buf_width, const int buf_height)

       The current implementation assumes the data at image is
       organized as a rectangle without a stride, So we pass the raw
       data to be processed, this is more data but correct.
    */
    if(darktable.gui->show_focus_peaking && mip == buf.size)
      dt_focuspeaking(cr, buf_wd, buf_ht, rgbbuf);

    cairo_surface_destroy(tmp_surface);
    cairo_destroy(cr);
  }

  // we consider skull/error as ok as the image hasn't to be reload
  if(buf_wd <= 30 && buf_ht <= 30)
    ret = DT_VIEW_SURFACE_OK;
  else if(mip != buf.size)
    ret = DT_VIEW_SURFACE_SMALLER;
  else
    ret = DT_VIEW_SURFACE_OK;

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  if(rgbbuf) free(rgbbuf);

  // logs
  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_LIGHTTABLE | DT_DEBUG_PERF,
             "got surface  %ix%i created in %0.04f sec",
             img_width, img_height, dt_get_wtime() - tt);

  // we consider skull as ok as the image hasn't to be reload
  if(ret != DT_VIEW_SURFACE_OK)
    dt_print(DT_DEBUG_LIGHTTABLE,
      "dt_view_image_get_surface  ID=%i with surface problem %s%s%s",
      imgid,
      tmp_surface ? "" : "no tmp_surface, ",
      ret == DT_VIEW_SURFACE_SMALLER ? "DT_VIEW_SURFACE_SMALLER" : "",
      ret == DT_VIEW_SURFACE_KO ? "DT_VIEW_SURFACE_KO" : "");
  return ret;
}

char* dt_view_extend_modes_str(const char * name,
                               const gboolean is_hdr,
                               const gboolean is_bw,
                               const gboolean is_bw_flow)
{
  char* upcase = g_ascii_strup(name, -1);  // extension in capital
                                           // letters to avoid
                                           // character descenders
  // convert to canonical format extension
  if(0 == g_ascii_strcasecmp(upcase, "JPG"))
  {
      gchar* canonical = g_strdup("JPEG");
      g_free(upcase);
      upcase = canonical;
  }
  else if(0 == g_ascii_strcasecmp(upcase, "HDR"))
  {
      gchar* canonical = g_strdup("RGBE");
      g_free(upcase);
      upcase = canonical;
  }
  else if(0 == g_ascii_strcasecmp(upcase, "TIF"))
  {
      gchar* canonical = g_strdup("TIFF");
      g_free(upcase);
      upcase = canonical;
  }

  if(is_hdr)
  {
    gchar* fullname = g_strdup_printf("%s HDR", upcase);
    g_free(upcase);
    upcase = fullname;
  }
  if(is_bw)
  {
    gchar* fullname = g_strdup_printf("%s B&W", upcase);
    g_free(upcase);
    upcase = fullname;
    if(!is_bw_flow)
    {
      fullname = g_strdup_printf("%s-", upcase);
      g_free(upcase);
      upcase = fullname;
    }
  }

  return upcase;
}

/**
 * \brief Set the selection bit to a given value for the specified image
 * \param[in] imgid The image id
 * \param[in] value The boolean value for the bit
 */
void dt_view_set_selection(const dt_imgid_t imgid,
                           const int value)
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
      DT_DEBUG_SQLITE3_CLEAR_BINDINGS
        (darktable.view_manager->statements.delete_from_selected);
      DT_DEBUG_SQLITE3_RESET
        (darktable.view_manager->statements.delete_from_selected);

      /* setup statement and execute */
      DT_DEBUG_SQLITE3_BIND_INT
        (darktable.view_manager->statements.delete_from_selected, 1, imgid);
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
void dt_view_toggle_selection(const dt_imgid_t imgid)
{
  /* clear and reset statement */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(darktable.view_manager->statements.is_selected);
  DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.is_selected);

  /* setup statement and iterate over rows */
  DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.is_selected, 1, imgid);
  if(sqlite3_step(darktable.view_manager->statements.is_selected) == SQLITE_ROW)
  {
    /* clear and reset statement */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS
      (darktable.view_manager->statements.delete_from_selected);
    DT_DEBUG_SQLITE3_RESET(darktable.view_manager->statements.delete_from_selected);

    /* setup statement and execute */
    DT_DEBUG_SQLITE3_BIND_INT(darktable.view_manager->statements.delete_from_selected,
                              1, imgid);
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

dt_view_type_flags_t dt_view_get_current(void)
{
  if(!darktable.view_manager) return DT_VIEW_LIGHTTABLE;

  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(!cv || ! cv->view) return DT_VIEW_LIGHTTABLE;

  return cv->view(cv);
}

/**
 * \brief Reset filter
 */
void dt_view_filtering_reset(const dt_view_manager_t *vm,
                             const gboolean smart_filter)
{
  if(vm->proxy.module_filtering.module && vm->proxy.module_filtering.reset_filter)
    vm->proxy.module_filtering.reset_filter
      (vm->proxy.module_filtering.module, smart_filter);
}

void dt_view_filtering_show_pref_menu(const dt_view_manager_t *vm,
                                      GtkWidget *bt)
{
  if(vm->proxy.module_filtering.module && vm->proxy.module_filtering.show_pref_menu)
    vm->proxy.module_filtering.show_pref_menu(vm->proxy.module_filtering.module, bt);
}

GtkWidget *dt_view_filter_get_filters_box(const dt_view_manager_t *vm)
{
  if(vm->proxy.filter.module && vm->proxy.filter.get_filter_box)
    return vm->proxy.filter.get_filter_box(vm->proxy.filter.module);
  return NULL;
}
GtkWidget *dt_view_filter_get_sort_box(const dt_view_manager_t *vm)
{
  if(vm->proxy.filter.module && vm->proxy.filter.get_sort_box)
    return vm->proxy.filter.get_sort_box(vm->proxy.filter.module);
  return NULL;
}

GtkWidget *dt_view_filter_get_count(const dt_view_manager_t *vm)
{
  if(vm && vm->proxy.filter.module && vm->proxy.filter.get_count)
    return vm->proxy.filter.get_count(vm->proxy.filter.module);
  return NULL;
}

void dt_view_active_images_reset(const gboolean raise)
{
  if(!darktable.view_manager->active_images)
    return;
  g_slist_free(darktable.view_manager->active_images);
  darktable.view_manager->active_images = NULL;

  if(raise)
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
}
void dt_view_active_images_add(dt_imgid_t imgid, gboolean raise)
{
  darktable.view_manager->active_images =
    g_slist_append(darktable.view_manager->active_images, GINT_TO_POINTER(imgid));
  if(raise)
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
}
GSList *dt_view_active_images_get()
{
  return darktable.view_manager->active_images;
}

void dt_view_manager_view_toolbox_add(dt_view_manager_t *vm,
                                      GtkWidget *tool,
                                      const dt_view_type_flags_t views)
{
  if(vm->proxy.view_toolbox.module)
    vm->proxy.view_toolbox.add(vm->proxy.view_toolbox.module, tool, views);
}

void dt_view_manager_module_toolbox_add(dt_view_manager_t *vm,
                                        GtkWidget *tool,
                                        const dt_view_type_flags_t views)
{
  if(vm->proxy.module_toolbox.module)
    vm->proxy.module_toolbox.add(vm->proxy.module_toolbox.module, tool, views);
}

dt_darkroom_layout_t dt_view_darkroom_get_layout(dt_view_manager_t *vm)
{
  if(vm->proxy.darkroom.view)
    return vm->proxy.darkroom.get_layout(vm->proxy.darkroom.view);
  else
    return DT_DARKROOM_LAYOUT_EDITING;
}

void dt_view_lighttable_set_zoom(dt_view_manager_t *vm,
                                 const gint zoom)
{
  if(vm->proxy.lighttable.module)
    vm->proxy.lighttable.set_zoom(vm->proxy.lighttable.module, zoom);
}

gint dt_view_lighttable_get_zoom(dt_view_manager_t *vm)
{
  if(vm->proxy.lighttable.module)
    return vm->proxy.lighttable.get_zoom(vm->proxy.lighttable.module);
  else
    return 10;
}

void dt_view_lighttable_culling_init_mode(dt_view_manager_t *vm)
{
  if(vm->proxy.lighttable.module)
    vm->proxy.lighttable.culling_init_mode(vm->proxy.lighttable.view);
}

void dt_view_lighttable_culling_preview_refresh(dt_view_manager_t *vm)
{
  if(vm->proxy.lighttable.module)
    vm->proxy.lighttable.culling_preview_refresh(vm->proxy.lighttable.view);
}

void dt_view_lighttable_culling_preview_reload_overlays(dt_view_manager_t *vm)
{
  if(vm->proxy.lighttable.module)
    vm->proxy.lighttable.culling_preview_reload_overlays(vm->proxy.lighttable.view);
}

dt_lighttable_layout_t dt_view_lighttable_get_layout(dt_view_manager_t *vm)
{
  if(vm->proxy.lighttable.module)
    return vm->proxy.lighttable.get_layout(vm->proxy.lighttable.module);
  else
    return DT_LIGHTTABLE_LAYOUT_FILEMANAGER;
}

gboolean dt_view_lighttable_preview_state(dt_view_manager_t *vm)
{
  if(vm->proxy.lighttable.module)
    return vm->proxy.lighttable.get_preview_state(vm->proxy.lighttable.view);
  else
    return FALSE;
}

void dt_view_lighttable_set_preview_state(dt_view_manager_t *vm,
                                          const gboolean state,
                                          const gboolean sticky,
                                          const gboolean focus)
{
  if(vm->proxy.lighttable.module)
    vm->proxy.lighttable.set_preview_state(vm->proxy.lighttable.view, state, sticky, focus);
}

void dt_view_lighttable_change_offset(dt_view_manager_t *vm,
                                      const gboolean reset,
                                      const dt_imgid_t imgid)
{
  if(vm->proxy.lighttable.module)
    vm->proxy.lighttable.change_offset(vm->proxy.lighttable.view, reset, imgid);
}

void dt_view_collection_update(const dt_view_manager_t *vm)
{
  if(vm->proxy.module_filtering.module)
    vm->proxy.module_filtering.update(vm->proxy.module_filtering.module);
  if(vm->proxy.module_collect.module)
    vm->proxy.module_collect.update(vm->proxy.module_collect.module);
}

void dt_view_filtering_set_sort(const dt_view_manager_t *vm,
                                const int sort,
                                const gboolean asc)
{
  if(vm->proxy.module_filtering.module)
    vm->proxy.module_filtering.set_sort(vm->proxy.module_filtering.module, sort, asc);
}

int32_t dt_view_tethering_get_selected_imgid(const dt_view_manager_t *vm)
{
  if(vm->proxy.tethering.view)
    return vm->proxy.tethering.get_selected_imgid(vm->proxy.tethering.view);

  return -1;
}

void dt_view_tethering_set_job_code(const dt_view_manager_t *vm,
                                    const char *name)
{
  if(vm->proxy.tethering.view)
    vm->proxy.tethering.set_job_code(vm->proxy.tethering.view, name);
}

const char *dt_view_tethering_get_job_code(const dt_view_manager_t *vm)
{
  if(vm->proxy.tethering.view)
    return vm->proxy.tethering.get_job_code(vm->proxy.tethering.view);
  return NULL;
}

#ifdef HAVE_MAP
void dt_view_map_center_on_location(const dt_view_manager_t *vm,
                                    const gdouble lon,
                                    const gdouble lat,
                                    const gdouble zoom)
{
  if(vm->proxy.map.view)
    vm->proxy.map.center_on_location(vm->proxy.map.view, lon, lat, zoom);
}

void dt_view_map_center_on_bbox(const dt_view_manager_t *vm,
                                const gdouble lon1,
                                const gdouble lat1,
                                const gdouble lon2,
                                const gdouble lat2)
{
  if(vm->proxy.map.view)
    vm->proxy.map.center_on_bbox(vm->proxy.map.view, lon1, lat1, lon2, lat2);
}

void dt_view_map_show_osd(const dt_view_manager_t *vm)
{
  if(vm->proxy.map.view)
    vm->proxy.map.show_osd(vm->proxy.map.view);
}

void dt_view_map_set_map_source(const dt_view_manager_t *vm,
                                const OsmGpsMapSource_t map_source)
{
  if(vm->proxy.map.view)
    vm->proxy.map.set_map_source(vm->proxy.map.view, map_source);
}

GObject *dt_view_map_add_marker(const dt_view_manager_t *vm,
                                const dt_geo_map_display_t type,
                                GList *points)
{
  if(vm->proxy.map.view)
    return vm->proxy.map.add_marker(vm->proxy.map.view, type, points);
  return NULL;
}

gboolean dt_view_map_remove_marker(const dt_view_manager_t *vm,
                                   const dt_geo_map_display_t type,
                                   GObject *marker)
{
  if(vm->proxy.map.view)
    return vm->proxy.map.remove_marker(vm->proxy.map.view, type, marker);
  return FALSE;
}
void dt_view_map_add_location(const dt_view_manager_t *vm,
                              dt_map_location_data_t *p,
                              const guint posid)
{
  if(vm->proxy.map.view)
    vm->proxy.map.add_location(vm->proxy.map.view, p, posid);
}

void dt_view_map_location_action(const dt_view_manager_t *vm,
                                 const int action)
{
  if(vm->proxy.map.view)
    vm->proxy.map.location_action(vm->proxy.map.view, action);
}

void dt_view_map_drag_set_icon(const dt_view_manager_t *vm,
                               GdkDragContext *context,
                               const dt_imgid_t imgid,
                               const int count)
{
  if(vm->proxy.map.view)
    vm->proxy.map.drag_set_icon(vm->proxy.map.view, context, imgid, count);
}

#endif

#ifdef HAVE_PRINT
void dt_view_print_settings(const dt_view_manager_t *vm,
                            dt_print_info_t *pinfo,
                            dt_images_box *imgs)
{
  if(vm->proxy.print.view)
    vm->proxy.print.print_settings(vm->proxy.print.view, pinfo, imgs);
}
#endif

GSList *dt_mouse_action_create_simple(GSList *actions,
                                      const dt_mouse_action_type_t type,
                                      const GdkModifierType accel,
                                      const char *const description)
{
  dt_mouse_action_t *a = calloc(1, sizeof(dt_mouse_action_t));
  if(a)
  {
    a->action = type;
    a->mods = accel;
    g_strlcpy(a->name, description, sizeof(a->name));
    return g_slist_append(actions, a);
  }
  else
    return actions;
}

GSList *dt_mouse_action_create_format(GSList *actions,
                                      const dt_mouse_action_type_t type,
                                      const GdkModifierType accel,
                                      const char *const format_string,
                                      const char *const replacement)
{
  dt_mouse_action_t *a = calloc(1, sizeof(dt_mouse_action_t));
  if(a)
  {
    a->action = type;
    a->mods = accel;
    g_snprintf(a->name, sizeof(a->name), format_string, replacement);
    return g_slist_append(actions, a);
  }
  else
    return actions;
}

static gchar *_mouse_action_get_string(dt_mouse_action_t *ma)
{
  gchar *atxt = NULL;
  if(ma->mods & GDK_SHIFT_MASK  )
    dt_util_str_cat(&atxt, "%s+", _("shift"));
  if(ma->mods & GDK_CONTROL_MASK)
    dt_util_str_cat(&atxt, "%s+", _("ctrl"));
  if(ma->mods & GDK_MOD1_MASK   )
#ifdef __APPLE__
    dt_util_str_cat(&atxt, "%s+", _("option"));
#else
    dt_util_str_cat(&atxt, "%s+", _("alt"));
#endif

  switch(ma->action)
  {
    case DT_MOUSE_ACTION_LEFT:
      dt_util_str_cat(&atxt, _("left-click"));
      break;
    case DT_MOUSE_ACTION_RIGHT:
      dt_util_str_cat(&atxt, _("right-click"));
      break;
    case DT_MOUSE_ACTION_MIDDLE:
      dt_util_str_cat(&atxt, _("middle-click"));
      break;
    case DT_MOUSE_ACTION_SCROLL:
      dt_util_str_cat(&atxt, _("scroll"));
      break;
    case DT_MOUSE_ACTION_DOUBLE_LEFT:
      dt_util_str_cat(&atxt, _("left double-click"));
      break;
    case DT_MOUSE_ACTION_DOUBLE_RIGHT:
      dt_util_str_cat(&atxt, _("right double-click"));
      break;
    case DT_MOUSE_ACTION_DRAG_DROP:
      dt_util_str_cat(&atxt, _("drag and drop"));
      break;
    case DT_MOUSE_ACTION_LEFT_DRAG:
      dt_util_str_cat(&atxt, _("left-click+drag"));
      break;
    case DT_MOUSE_ACTION_RIGHT_DRAG:
      dt_util_str_cat(&atxt, _("right-click+drag"));
      break;
  }

  return atxt;
}

static void _accels_window_destroy(GtkWidget *widget, dt_view_manager_t *vm)
{
  // set to NULL so we can rely on it after
  vm->accels_window.window = NULL;
}

static void _accels_window_sticky(GtkWidget *widget,
                                  GdkEventButton *event,
                                  dt_view_manager_t *vm)
{
  if(!vm->accels_window.window) return;

  // creating new window
  GtkWindow *win = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
  dt_gui_add_class(GTK_WIDGET(win), "dt_accels_window");
  gtk_window_set_title(win, _("darktable - accels window"));
  GtkAllocation alloc;
  gtk_widget_get_allocation(dt_ui_main_window(darktable.gui->ui), &alloc);

  gtk_window_set_resizable(win, TRUE);
  gtk_window_set_icon_name(win, "darktable");
  gtk_window_set_default_size(win, alloc.width * 0.7, alloc.height * 0.7);
  g_signal_connect(win, "destroy", G_CALLBACK(_accels_window_destroy), vm);

  GtkWidget *sw = dt_gui_container_first_child(GTK_CONTAINER(vm->accels_window.window));
  g_object_ref(sw);
  gtk_container_remove(GTK_CONTAINER(vm->accels_window.window), sw);
  gtk_container_add(GTK_CONTAINER(win), sw);
  g_object_unref(sw);

  gtk_widget_destroy(vm->accels_window.window);
  vm->accels_window.window = GTK_WIDGET(win);
  gtk_widget_show_all(vm->accels_window.window);
  gtk_widget_hide(vm->accels_window.sticky_btn);

  vm->accels_window.sticky = TRUE;
}

void dt_view_accels_show(dt_view_manager_t *vm)
{
  if(vm->accels_window.window) return;

  vm->accels_window.sticky = FALSE;
  vm->accels_window.prevent_refresh = FALSE;
  vm->accels_window.window = gtk_window_new(GTK_WINDOW_POPUP);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(vm->accels_window.window);
#endif
  dt_gui_add_class(vm->accels_window.window, "dt_accels_window");

  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);

  GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

  vm->accels_window.flow_box = gtk_flow_box_new();
  dt_gui_add_class(vm->accels_window.flow_box, "dt_accels_box");
  gtk_orientable_set_orientation(GTK_ORIENTABLE(vm->accels_window.flow_box),
                                 GTK_ORIENTATION_HORIZONTAL);

  gtk_box_pack_start(GTK_BOX(hb), vm->accels_window.flow_box, TRUE, TRUE, 0);

  GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  vm->accels_window.sticky_btn = dtgtk_button_new(dtgtk_cairo_paint_multiinstance, 0, NULL);
  gtk_widget_set_tooltip_text(vm->accels_window.sticky_btn,
                              _("switch to a classic window which will stay open after key release"));
  g_signal_connect(G_OBJECT(vm->accels_window.sticky_btn), "button-press-event",
                   G_CALLBACK(_accels_window_sticky),
                   vm);
  dt_gui_add_class(vm->accels_window.sticky_btn, "dt_accels_stick");
  gtk_box_pack_start(GTK_BOX(vb), vm->accels_window.sticky_btn, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hb), vb, FALSE, FALSE, 0);

  dt_view_accels_refresh(vm);

  GtkAllocation alloc;
  gtk_widget_get_allocation(dt_ui_main_window(darktable.gui->ui), &alloc);
  // gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), alloc.height);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), alloc.height);
  gtk_scrolled_window_set_max_content_width(GTK_SCROLLED_WINDOW(sw), alloc.width);
  gtk_container_add(GTK_CONTAINER(sw), hb);
  gtk_container_add(GTK_CONTAINER(vm->accels_window.window), sw);

  gtk_window_set_resizable(GTK_WINDOW(vm->accels_window.window), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(vm->accels_window.window),
                              alloc.width, alloc.height);
  gtk_window_set_transient_for(GTK_WINDOW(vm->accels_window.window),
                               GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_window_set_keep_above(GTK_WINDOW(vm->accels_window.window), TRUE);
  // needed on macOS to avoid fullscreening the popup with newer GTK
  gtk_window_set_type_hint(GTK_WINDOW(vm->accels_window.window),
                           GDK_WINDOW_TYPE_HINT_POPUP_MENU);

  gtk_window_set_gravity(GTK_WINDOW(vm->accels_window.window), GDK_GRAVITY_STATIC);
  gtk_window_set_position(GTK_WINDOW(vm->accels_window.window),
                          GTK_WIN_POS_CENTER_ON_PARENT);
  gtk_widget_show_all(vm->accels_window.window);
}

void dt_view_accels_hide(dt_view_manager_t *vm)
{
  if(vm->accels_window.window && vm->accels_window.sticky)
    return;
  if(vm->accels_window.window)
    gtk_widget_destroy(vm->accels_window.window);
  vm->accels_window.window = NULL;
}

void dt_view_accels_refresh(dt_view_manager_t *vm)
{
  if(!vm->accels_window.window || vm->accels_window.prevent_refresh)
    return;

  // drop all existing tables
  GList *lw = gtk_container_get_children(GTK_CONTAINER(vm->accels_window.flow_box));

  for(const GList *lw_iter = lw; lw_iter; lw_iter = g_list_next(lw_iter))
  {
    GtkWidget *w = (GtkWidget *)lw_iter->data;
    gtk_widget_destroy(w);
  }
  g_list_free(lw);

  // get the list of valid accel for this view
  const dt_view_t *cv = dt_view_manager_get_current_view(vm);
  const dt_view_type_flags_t v = cv->view(cv);

  GHashTable *blocks = dt_shortcut_category_lists(v);

  dt_action_t *first_category = darktable.control->actions;

  // we add the mouse actions too
  dt_action_t mouse_actions = { .label = _("mouse actions"),
                                .next = first_category };
  if(cv->mouse_actions)
  {
    GtkListStore *mouse_list = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    g_hash_table_insert(blocks, &mouse_actions, mouse_list);

    GSList *actions = cv->mouse_actions(cv);
    for(GSList *lm = actions; lm; lm = g_slist_next(lm))
    {
      dt_mouse_action_t *ma = lm->data;
      if(ma)
      {
        gchar *atxt = _mouse_action_get_string(ma);
        gtk_list_store_insert_with_values(mouse_list, NULL, -1, 0, atxt, 1, ma->name, -1);
        g_free(atxt);
      }
    }
    g_slist_free_full(actions, g_free);

    first_category = &mouse_actions;
  }

  // now we create and insert the widget to display all accels by categories
  for(dt_action_t *category = first_category; category; category = category->next)
  {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    // the title
    GtkWidget *lb = gtk_label_new(category->label);
    dt_gui_add_class(lb, "dt_accels_cat_title");
    gtk_box_pack_start(GTK_BOX(box), lb, FALSE, FALSE, 0);

    // the list of accels
    GtkTreeModel *model = GTK_TREE_MODEL(g_hash_table_lookup(blocks, category));
    if(model)
    {
      GtkWidget *list = gtk_tree_view_new_with_model(model);
      g_object_unref(model);
      GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
      GtkTreeViewColumn *column =
        gtk_tree_view_column_new_with_attributes(_("shortcut"), renderer, "text", 0, NULL);
      gtk_tree_view_append_column(GTK_TREE_VIEW(list), column);
      column = gtk_tree_view_column_new_with_attributes(_("action"),
                                                        renderer, "text", 1, NULL);
      gtk_tree_view_append_column(GTK_TREE_VIEW(list), column);

      gtk_box_pack_start(GTK_BOX(box), list, FALSE, FALSE, 0);

      gtk_flow_box_insert(GTK_FLOW_BOX(vm->accels_window.flow_box), box, -1);
    }
  }

  g_hash_table_destroy(blocks);

  gtk_widget_show_all(vm->accels_window.flow_box);
}

static void _audio_child_watch(const GPid pid,
                               const gint status,
                               gpointer data)
{
  dt_view_manager_t *vm = (dt_view_manager_t *)data;
  vm->audio.audio_player_id = -1;
  g_spawn_close_pid(pid);
}

void dt_view_audio_start(dt_view_manager_t *vm,
                         const dt_imgid_t imgid)
{
  char *player = dt_conf_get_string("plugins/lighttable/audio_player");
  if(player && *player)
  {
    char *filename = dt_image_get_audio_path(imgid);
    if(filename)
    {
      char *argv[] = { player, filename, NULL };
      gboolean ret = g_spawn_async(NULL, argv, NULL,
                                   G_SPAWN_DO_NOT_REAP_CHILD
                                   | G_SPAWN_SEARCH_PATH
                                   | G_SPAWN_STDOUT_TO_DEV_NULL
                                   | G_SPAWN_STDERR_TO_DEV_NULL,
                                   NULL, NULL,
                                   &vm->audio.audio_player_pid, NULL);

      if(ret)
      {
        vm->audio.audio_player_id = imgid;
        vm->audio.audio_player_event_source =
          g_child_watch_add(vm->audio.audio_player_pid,
                            (GChildWatchFunc)_audio_child_watch, vm);
      }
      else
        vm->audio.audio_player_id = -1;

      g_free(filename);
    }
  }
  g_free(player);
}

void dt_view_audio_stop(dt_view_manager_t *vm)
{
  // make sure that the process didn't finish yet and that _audio_child_watch() hasn't run
  if(vm->audio.audio_player_id == -1)
    return;

  // we don't want to trigger the callback due to a possible race condition
  g_source_remove(vm->audio.audio_player_event_source);
#ifdef _WIN32
// TODO: add Windows code to actually kill the process
#else  // _WIN32
  if(vm->audio.audio_player_id != -1)
  {
    if(getpgid(0) != getpgid(vm->audio.audio_player_pid))
      kill(-vm->audio.audio_player_pid, SIGKILL);
    else
      kill(vm->audio.audio_player_pid, SIGKILL);
  }
#endif // _WIN32
  g_spawn_close_pid(vm->audio.audio_player_pid);
  vm->audio.audio_player_id = -1;
}

void dt_view_paint_surface(cairo_t *cr,
                           const size_t width,
                           const size_t height,
                           dt_dev_viewport_t *port,
                           const dt_window_t window,
                           uint8_t *buf,
                           float buf_scale,
                           int buf_width,
                           int buf_height,
                           float buf_zoom_x,
                           float buf_zoom_y)
{
  dt_develop_t *dev = darktable.develop;

  int processed_width, processed_height;
  dt_dev_get_processed_size(port, &processed_width, &processed_height);
  if(!processed_width || !processed_height)
    return;

  float pts[] = { buf_zoom_x, buf_zoom_y,
                  dev->preview_pipe->backbuf_zoom_x, dev->preview_pipe->backbuf_zoom_y,
                  port->zoom_x, port->zoom_y };
  dt_dev_distort_transform_plus(dev, port->pipe, 0.0f, DT_DEV_TRANSFORM_DIR_ALL, pts, 3);

  float offset_x = pts[0] / processed_width - 0.5f;
  float offset_y = pts[1] / processed_height - 0.5f;
  float preview_x = pts[2] / processed_width - 0.5f;
  float preview_y = pts[3] / processed_height - 0.5f;
  float zoom_x = pts[4] / port->pipe->processed_width - 0.5f;
  float zoom_y = pts[5] / port->pipe->processed_height - 0.5f;

  dt_dev_zoom_t zoom = port->zoom;
  int closeup = port->closeup;
  const float ppd           = port->ppd;
  const double tb           = port->border_size;
  const float zoom_scale    = dt_dev_get_zoom_scale(port, zoom, 1<<closeup, 1);
  const float backbuf_scale = dt_dev_get_zoom_scale(port, zoom, 1.0f, 0) * ppd;

  dt_print_pipe(DT_DEBUG_EXPOSE,
      "dt_view_paint_surface",
        port->pipe, NULL, DT_DEVICE_NONE, NULL, NULL,
        "viewport zoom_scale %6.3f backbuf_scale %6.3f "
        "(x=%6.2f y=%6.2f) -> (x=%+.3f y=%+.3f)",
        zoom_scale, backbuf_scale,
        port->zoom_x, port->zoom_y, zoom_x, zoom_y);

  cairo_save(cr);

  if(port->iso_12646)
  {
    // force middle grey in background
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_ISO12646_BG);
  }
  else
  {
    if(dev->full_preview)
      dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_PREVIEW_BG);
    else
      dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_DARKROOM_BG);
  }

  cairo_paint(cr);

  cairo_translate(cr, 0.5 * width, 0.5 * height);

  dt_pthread_mutex_lock(&dev->preview_pipe->backbuf_mutex);

  const int maxw = MIN(port->width, backbuf_scale * processed_width * (1<<closeup) / ppd);
  const int maxh = MIN(port->height, backbuf_scale * processed_height * (1<<closeup) / ppd);

  if(port->iso_12646
     && window != DT_WINDOW_SLIDESHOW)
  {
    // draw the white frame around picture
    const double ratio = dt_conf_get_float("darkroom/ui/iso12464_ratio") * 2;
    const double borw = maxw + tb * ratio, borh = maxh + tb * ratio;
    cairo_rectangle(cr, -0.5 * borw, -0.5 * borh, borw, borh);
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_ISO12646_FG);
    cairo_fill(cr);
  }

  cairo_rectangle(cr, -0.5 * maxw, -0.5 * maxh, maxw, maxh);
  cairo_clip(cr);

  cairo_scale(cr, zoom_scale, zoom_scale);

  const double back_scale = (buf_scale == 0 ? 1.0 : backbuf_scale / buf_scale) * (1<<closeup) / ppd;
  const double trans_x = (offset_x - zoom_x) * processed_width * buf_scale - 0.5 * buf_width;
  const double trans_y = (offset_y - zoom_y) * processed_height * buf_scale - 0.5 * buf_height;

  if(dev->preview_pipe->output_imgid == dev->image_storage.id
     && (port->pipe->output_imgid != dev->image_storage.id
         || fabsf(backbuf_scale / buf_scale - 1.0f) > .09f
         || floor(maxw / 2 / back_scale) - 1 > MIN(- trans_x, trans_x + buf_width)
         || floor(maxh / 2 / back_scale) - 1 > MIN(- trans_y, trans_y + buf_height))
     && (port == &dev->full || port == &dev->preview2))
  {
    if(port->pipe->status == DT_DEV_PIXELPIPE_VALID)
      port->pipe->status = DT_DEV_PIXELPIPE_DIRTY;

    // draw preview
    float wd = processed_width * dev->preview_pipe->processed_width / MAX(1, dev->full.pipe->processed_width);
    float ht = processed_height * dev->preview_pipe->processed_width / MAX(1, dev->full.pipe->processed_width);

    cairo_surface_t *preview = dt_view_create_surface(dev->preview_pipe->backbuf,
                                                      dev->preview_pipe->backbuf_width,
                                                      dev->preview_pipe->backbuf_height);
    cairo_set_source_surface(cr, preview, (preview_x - zoom_x) * wd - 0.5 * dev->preview_pipe->backbuf_width,
                                          (preview_y - zoom_y) * ht - 0.5 * dev->preview_pipe->backbuf_height);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
    cairo_paint(cr);

    dt_print_pipe(DT_DEBUG_EXPOSE,
        "  painting",
         dev->preview_pipe, NULL, DT_DEVICE_NONE, NULL, NULL,
         "size %4lux%-4lu processed %4.0fx%-4.0f "
         "buf %4dx%-4d scale=%.3f "
         "zoom (x=%6.2f y=%6.2f) -> offset (x=%+.3f y=%+.3f)",
         width, height, wd, ht,
         dev->preview_pipe->backbuf_width, dev->preview_pipe->backbuf_height, zoom_scale,
         dev->preview_pipe->backbuf_zoom_x, dev->preview_pipe->backbuf_zoom_y,
         preview_x, preview_y);
    cairo_surface_destroy(preview);
  }

  dt_pthread_mutex_unlock(&dev->preview_pipe->backbuf_mutex);

  if(port->pipe->output_imgid == dev->image_storage.id
     || dev->preview_pipe->output_imgid != dev->image_storage.id)
  {
    dt_print_pipe(DT_DEBUG_EXPOSE,
        "  painting",
         port->pipe, NULL, DT_DEVICE_NONE, NULL, NULL,
         "size %4lux%-4lu processed %4dx%-4d "
         "buf %4dx%-4d scale=%.3f "
         "zoom (x=%6.2f y=%6.2f) -> offset (x=%+.3f y=%+.3f)",
         width, height, processed_width, processed_height,
         buf_width, buf_height, buf_scale,
         buf_zoom_x, buf_zoom_y,
         offset_x, offset_y);
    cairo_scale(cr, back_scale / zoom_scale, back_scale / zoom_scale);
    cairo_translate(cr, trans_x, trans_y);
    cairo_surface_t *surface = dt_view_create_surface(buf, buf_width, buf_height);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
    cairo_paint(cr);

    if(darktable.gui->show_focus_peaking
      && window != DT_WINDOW_SLIDESHOW)
    {
      dt_focuspeaking(cr, buf_width, buf_height,
                      cairo_image_surface_get_data(surface));
    }
    cairo_surface_destroy(surface);
  }

  cairo_restore(cr);
}

cairo_surface_t *dt_view_create_surface(uint8_t *buffer,
                                        const size_t processed_width,
                                        const size_t processed_height)
{
  const int32_t stride =
    cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, processed_width);
  return cairo_image_surface_create_for_data
    (buffer, CAIRO_FORMAT_RGB24, processed_width, processed_height, stride);
}

dt_view_context_t dt_view_get_context_hash(void)
{
  dt_develop_t *dev = darktable.develop;
  dt_dev_zoom_t zoom;
  int closeup;
  float zoom_x, zoom_y;
  dt_dev_get_viewport_params(&dev->full, &zoom, &closeup, &zoom_x, &zoom_y);
  const float zoom_scale = dt_dev_get_zoom_scale(&dev->full, zoom, 1<<closeup, 1);

  // calculate a hash on view parameters. Use flt_prec here to avoid different hashes
  // for irrelevant variations for the zooms.
  const float flt_prec = 1.e6;
  const uint32_t test[] = { (uint32_t)dev->full.iso_12646,
                            (uint32_t)darktable.gui->show_focus_peaking,
                            (uint32_t)closeup,
                            (uint32_t)(zoom_scale * flt_prec),
                            (uint32_t)(zoom_x * flt_prec),
                            (uint32_t)(zoom_y * flt_prec),
                            (uint32_t)(dev->late_scaling.enabled) };

  return (dt_view_context_t)dt_hash(DT_INITHASH, &test, sizeof(test));
}

gboolean dt_view_check_context_hash(dt_view_context_t *ctx)
{
  const dt_view_context_t curctx = dt_view_get_context_hash();
  if(curctx == *ctx)
  {
    return TRUE;
  }
  else
  {
    *ctx = curctx;
    return FALSE;
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
