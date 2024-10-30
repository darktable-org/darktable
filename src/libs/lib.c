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

#include "libs/lib.h"
#include "common/debug.h"
#include "common/module.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/expander.h"
#include "dtgtk/icon.h"
#include "gui/accelerators.h"
#include "gui/drag_and_drop.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <stdbool.h>
#include <stdlib.h>

typedef struct dt_lib_presets_edit_dialog_t
{
  GtkEntry *name, *description;
  char plugin_name[128];
  int32_t version;
  void *params;
  int32_t params_size;
  gchar *original_name;
  dt_lib_module_t *module;
  gint old_id;
} dt_lib_presets_edit_dialog_t;

static gchar *_get_lib_view_path(const dt_lib_module_t *module,
                                 const dt_view_t *cv,
                                 char *suffix);

gboolean dt_lib_is_visible_in_view(dt_lib_module_t *module,
                                   const dt_view_t *view)
{
  if(!module->views)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "module %s doesn't have views flags",
             module->name(module));
    return FALSE;
  }

  gboolean ret = module->views(module) & view->view(view);
  gchar *key = _get_lib_view_path(module, view, "_visible");
  if(key && dt_conf_key_exists(key))
    ret = dt_conf_get_bool(key);
  g_free(key);

  return ret;
}

/** calls module->cleanup and closes the dl connection. */
static void dt_lib_unload_module(dt_lib_module_t *module);

gchar *dt_lib_get_active_preset_name(dt_lib_module_info_t *minfo)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT name, op_params, writeprotect"
      " FROM data.presets"
      " WHERE operation=?1 AND op_version=?2",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, minfo->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, minfo->version);
  gchar *name = NULL;
  // collect all presets for op from db
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    void *op_params = (void *)sqlite3_column_blob(stmt, 1);
    int32_t op_params_size = sqlite3_column_bytes(stmt, 1);
    if(op_params_size == minfo->params_size
       && !memcmp(minfo->params, op_params, op_params_size))
    {
      name = g_strdup((char *)sqlite3_column_text(stmt, 0));
      break;
    }
  }
  sqlite3_finalize(stmt);
  return name;
}

static void edit_preset(const char *name_in,
                        dt_lib_module_info_t *minfo)
{
  // get the original name of the preset
  gchar *name = NULL;
  if(name_in == NULL)
  {
    name = dt_lib_get_active_preset_name(minfo);
    if(name == NULL) return;
  }
  else
    name = g_strdup(name_in);

  // find the rowid of the preset
  int rowid = -1;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT rowid"
                              " FROM data.presets"
                              " WHERE name = ?1 AND operation = ?2 AND op_version = ?3",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, minfo->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, minfo->version);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    rowid = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  // if we don't have a valid rowid, just exit, there's a problem !
  if(rowid < 0) return;

  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  dt_gui_presets_show_edit_dialog
    (name, minfo->plugin_name, rowid, NULL, NULL, TRUE, TRUE, FALSE,
     GTK_WINDOW(window));
}

static void menuitem_update_preset(GtkMenuItem *menuitem,
                                   dt_lib_module_info_t *minfo)
{
  char *name = g_object_get_data(G_OBJECT(menuitem), "dt-preset-name");

  if(!dt_conf_get_bool("plugins/lighttable/preset/ask_before_delete_preset")
     || dt_gui_show_yes_no_dialog(_("update preset?"),
                                  _("do you really want to update the preset `%s'?"),
                                  name))
  {
    // commit all the module fields
    sqlite3_stmt *stmt;
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE data.presets"
                                " SET op_version=?2, op_params=?3"
                                " WHERE name=?4 AND operation=?1",
                                -1, &stmt, NULL);
    // clang-format on

    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, minfo->plugin_name, -1,
                               SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, minfo->version);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, minfo->params, minfo->params_size,
                               SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_PRESETS_CHANGED,
                            g_strdup(minfo->plugin_name));
  }
}

static void menuitem_new_preset(GtkMenuItem *menuitem,
                                dt_lib_module_info_t *minfo)
{
  dt_lib_presets_remove(_("new preset"), minfo->plugin_name, minfo->version);

  // add new preset
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "INSERT INTO data.presets (name, description, operation, op_version, op_params,"
      "  blendop_params, blendop_version, enabled, model, maker, lens,"
      "  iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max,"
      "  focal_length_min, focal_length_max, writeprotect, "
      "  autoapply, filter, def, format)"
      " VALUES (?1, '', ?2, ?3, ?4, NULL, 0, 1, '%', "
      "         '%', '%', 0, 340282346638528859812000000000000000000, 0, 100000000,"
      "          0, 100000000, 0, 1000, 0, 0, 0, 0, 0)",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, _("new preset"), -1, SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, minfo->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, minfo->version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, minfo->params, minfo->params_size, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  // create a shortcut for the new entry

  dt_action_define_preset(&minfo->module->actions, _("new preset"));

  // then show edit dialog
  edit_preset(_("new preset"), minfo);
}

static void menuitem_edit_preset(GtkMenuItem *menuitem,
                                 dt_lib_module_info_t *minfo)
{
  edit_preset(NULL, minfo);
}

static void menuitem_manage_presets(GtkMenuItem *menuitem,
                                    dt_lib_module_info_t *minfo)
{
  if(minfo->module->manage_presets) minfo->module->manage_presets(minfo->module);
}

static void menuitem_delete_preset(GtkMenuItem *menuitem,
                                   dt_lib_module_info_t *minfo)
{
  gchar *name = dt_lib_get_active_preset_name(minfo);
  if(name == NULL) return;

  if(!dt_conf_get_bool("plugins/lighttable/preset/ask_before_delete_preset")
     || dt_gui_show_yes_no_dialog(_("delete preset?"),
                                  _("do you really want to delete the preset `%s'?"),
                                  name))
  {
    dt_action_rename_preset(&minfo->module->actions, name, NULL);

    dt_lib_presets_remove(name, minfo->plugin_name, minfo->version);

    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_PRESETS_CHANGED,
                            g_strdup(minfo->plugin_name));
  }
  g_free(name);
}

gchar *dt_lib_presets_duplicate(const gchar *preset,
                                const gchar *module_name,
                                const int module_version)
{
  sqlite3_stmt *stmt;

  // find the new name
  int i = 0;
  gboolean ko = TRUE;
  while(ko)
  {
    i++;
    gchar *tx = g_strdup_printf("%s_%d", preset, i);
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get(darktable.db),
        "SELECT name"
        " FROM data.presets"
        " WHERE operation = ?1 AND op_version = ?2 AND name = ?3", -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module_version);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, tx, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(stmt) != SQLITE_ROW) ko = FALSE;
    sqlite3_finalize(stmt);
    g_free(tx);
  }
  gchar *nname = g_strdup_printf("%s_%d", preset, i);

  // and we duplicate the entry
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "INSERT INTO data.presets"
      " (name, description, operation, op_version, op_params, "
      "  blendop_params, blendop_version, enabled, model, maker, lens, "
      "  iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max, "
      "  focal_length_min, focal_length_max, writeprotect, "
      "  autoapply, filter, def, format) "
      "SELECT ?1, description, operation, op_version, op_params, "
      "  blendop_params, blendop_version, enabled, model, maker, lens, "
      "  iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max, "
      "  focal_length_min, focal_length_max, 0, "
      "  autoapply, filter, def, format"
      " FROM data.presets"
      " WHERE operation = ?2 AND op_version = ?3 AND name = ?4",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, nname, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, module_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, module_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, preset, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return nname;
}

void dt_lib_presets_remove(const gchar *preset,
                           const gchar *module_name,
                           const int module_version)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "DELETE FROM data.presets"
      " WHERE name=?1 AND operation=?2 AND op_version=?3 AND writeprotect=0", -1, &stmt,
      NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, preset, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, module_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, module_version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

gboolean dt_lib_presets_apply(const gchar *preset,
                              const gchar *module_name,
                              const int module_version)
{
  gboolean ret = TRUE;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT op_params, writeprotect"
      " FROM data.presets"
      " WHERE operation = ?1 AND op_version = ?2 AND name = ?3",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, preset, -1, SQLITE_TRANSIENT);

  int res = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *blob = sqlite3_column_blob(stmt, 0);
    int length = sqlite3_column_bytes(stmt, 0);
    int writeprotect = sqlite3_column_int(stmt, 1);
    if(blob)
    {
      for(const GList *it = darktable.lib->plugins; it; it = g_list_next(it))
      {
        dt_lib_module_t *module = it->data;
        if(!strncmp(module->plugin_name, module_name, 128))
        {
          gchar *tx = g_strdup_printf("plugins/darkroom/%s/last_preset", module_name);
          dt_conf_set_string(tx, preset);
          g_free(tx);
          res = module->set_params(module, blob, length);
          break;
        }
      }
    }

    if(!writeprotect) dt_gui_store_last_preset(preset);
  }
  else
    ret = FALSE;
  sqlite3_finalize(stmt);
  if(res)
  {
    dt_control_log(_("deleting preset for obsolete module"));
    dt_lib_presets_remove(preset, module_name, module_version);
  }
  return ret;
}

void dt_lib_presets_update(const gchar *preset,
                           const gchar *module_name,
                           const int module_version,
                           const gchar *newname,
                           const gchar *desc,
                           const void *params,
                           const int32_t params_size)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.presets"
                              " SET name = ?1, description = ?2, op_params = ?3"
                              " WHERE operation = ?4 AND op_version = ?5 AND name = ?6",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, newname, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, desc, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, module_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, module_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, preset, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static void _menuitem_activate_preset(GtkMenuItem *menuitem,
                                      dt_lib_module_info_t *minfo)
{
  GdkEvent *event = gtk_get_current_event();
  if(event->type == GDK_KEY_PRESS)
    dt_lib_presets_apply(g_object_get_data(G_OBJECT(menuitem), "dt-preset-name"),
                         minfo->plugin_name, minfo->version);
  gdk_event_free(event);
}

static gboolean _menuitem_button_preset(GtkMenuItem *menuitem,
                                        GdkEventButton *event,
                                        dt_lib_module_info_t *minfo)
{
  char *name = g_object_get_data(G_OBJECT(menuitem), "dt-preset-name");

  if(event->button == 1)
    dt_lib_presets_apply(name, minfo->plugin_name, minfo->version);
  else
    dt_shortcut_copy_lua((dt_action_t*)minfo->module, name);

  return FALSE;
}

static void free_module_info(GtkWidget *widget,
                             gpointer user_data)
{
  dt_lib_module_info_t *minfo = (dt_lib_module_info_t *)user_data;
  g_free(minfo->plugin_name);
  free(minfo->params);
  free(minfo);
}

static void dt_lib_presets_popup_menu_show(dt_lib_module_info_t *minfo,
                                           GtkWidget *w)
{
  GtkMenu *menu = GTK_MENU(gtk_menu_new());

  const gboolean hide_default = dt_conf_get_bool("plugins/lighttable/hide_default_presets");
  const gboolean default_first = dt_conf_get_bool("modules/default_presets_first");

  g_signal_connect(G_OBJECT(menu), "destroy", G_CALLBACK(free_module_info), minfo);

  GtkWidget *mi;
  int active_preset = -1, cnt = 0;
  gboolean selected_writeprotect = FALSE;
  sqlite3_stmt *stmt;
  // order like the pref value
  // clang-format off
  gchar *query = g_strdup_printf("SELECT name, op_params, writeprotect, description"
                                 " FROM data.presets"
                                 " WHERE operation=?1 AND op_version=?2"
                                 " ORDER BY writeprotect %s, LOWER(name), rowid",
                                 default_first ? "DESC" : "ASC");
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, minfo->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, minfo->version);
  g_free(query);

  // collect all presets for op from db
  int found = 0;
  int last_wp = -1;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    // default vs built-in stuff
    const gboolean writeprotect = sqlite3_column_int(stmt, 2);
    if(hide_default && writeprotect)
    {
      // skip default module if set to hide them.
      continue;
    }
    if(last_wp == -1)
    {
      last_wp = writeprotect;
    }
    else if(last_wp != writeprotect)
    {
      last_wp = writeprotect;
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    }

    void *op_params = (void *)sqlite3_column_blob(stmt, 1);
    int32_t op_params_size = sqlite3_column_bytes(stmt, 1);
    const char *name = (char *)sqlite3_column_text(stmt, 0);

    if(darktable.gui->last_preset
       && strcmp(darktable.gui->last_preset, name) == 0) found = 1;

    // selected in bold:
    // printf("comparing %d bytes to %d\n", op_params_size, minfo->params_size);
    // for(int k=0;k<op_params_size && !memcmp(minfo->params, op_params, k);k++)
    //   printf("compare [%c %c] %d:
    // %d\n",
    // ((const char*)(minfo->params))[k],
    // ((const char*)(op_params))[k],
    // k, memcmp(minfo->params, op_params, k));
    if(op_params_size == minfo->params_size
       && !memcmp(minfo->params, op_params, op_params_size))
    {
      active_preset = cnt;
      selected_writeprotect = writeprotect;
      mi = gtk_check_menu_item_new_with_label(name);
      dt_gui_add_class(mi, "dt_transparent_background");
      gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), TRUE);
      dt_gui_add_class(mi, "active_menu_item");
    }
    else
    {
      mi = gtk_menu_item_new_with_label((const char *)name);
    }
    g_object_set_data_full(G_OBJECT(mi), "dt-preset-name", g_strdup(name), g_free);
    g_object_set_data(G_OBJECT(mi), "dt-preset-module", minfo->module);

    g_signal_connect(G_OBJECT(mi), "activate",
                     G_CALLBACK(_menuitem_activate_preset), minfo);
    g_signal_connect(G_OBJECT(mi), "button-press-event",
                     G_CALLBACK(_menuitem_button_preset), minfo);
    gtk_widget_set_tooltip_text(mi, (const char *)sqlite3_column_text(stmt, 3));
    gtk_widget_set_has_tooltip(mi, TRUE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    cnt++;
  }
  sqlite3_finalize(stmt);

  if(cnt > 0)
  {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    cnt = 0;
  }

  if(minfo->module->manage_presets)
  {
    mi = gtk_menu_item_new_with_label(_("manage presets..."));
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_manage_presets), minfo);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    cnt++;
  }
  else if(active_preset >= 0) // FIXME: this doesn't seem to work.
  {
    if(!selected_writeprotect)
    {
      mi = gtk_menu_item_new_with_label(_("edit this preset.."));
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_edit_preset), minfo);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

      mi = gtk_menu_item_new_with_label(_("delete this preset"));
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_delete_preset), minfo);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
      cnt++;
    }
  }
  else
  {
    mi = gtk_menu_item_new_with_label(_("store new preset.."));
    if(minfo->params_size == 0)
    {
      gtk_widget_set_sensitive(mi, FALSE);
      gtk_widget_set_tooltip_text(mi, _("nothing to save"));
    }
    else
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_new_preset), minfo);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

    if(darktable.gui->last_preset && found)
    {
      char *markup = g_markup_printf_escaped("%s <span weight=\"bold\">%s</span>",
                                             _("update preset"),
                                             darktable.gui->last_preset);
      mi = gtk_menu_item_new_with_label("");
      gtk_widget_set_sensitive(mi, minfo->params_size > 0);
      gtk_label_set_markup(GTK_LABEL(gtk_bin_get_child(GTK_BIN(mi))), markup);
      g_object_set_data_full(G_OBJECT(mi), "dt-preset-name",
                             g_strdup(darktable.gui->last_preset), g_free);
      g_signal_connect(G_OBJECT(mi), "activate",
                       G_CALLBACK(menuitem_update_preset), minfo);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
      g_free(markup);
    }
    cnt++;
  }

  if(minfo->module->set_preferences)
  {
    if(cnt>0)
    {
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    }
    minfo->module->set_preferences(GTK_MENU_SHELL(menu), minfo->module);
  }

  dt_gui_menu_popup(menu, w, GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST);
}

static int _lib_position(const dt_lib_module_t *module)
{
  int pos = module->position ? module->position(module) + 1 : 0;
  
  gchar *key = _get_lib_view_path(module, NULL, "_position");
  if(key && dt_conf_key_exists(key))
    pos = dt_conf_get_int(key);
  g_free(key);

  return pos;
}

gint dt_lib_sort_plugins(gconstpointer a, gconstpointer b)
{
  return ABS(_lib_position(a)) - ABS(_lib_position(b));
}

uint32_t dt_lib_get_container(dt_lib_module_t *module)
{
  uint32_t container = module->container(module);

  if(_lib_position(module) < 0)
    container = container == DT_UI_CONTAINER_PANEL_LEFT_CENTER
              ? DT_UI_CONTAINER_PANEL_RIGHT_CENTER
              : DT_UI_CONTAINER_PANEL_LEFT_CENTER;

  if(container == DT_UI_CONTAINER_PANEL_RIGHT_CENTER
     && dt_view_get_current() == DT_VIEW_DARKROOM)
    container = DT_UI_CONTAINER_PANEL_LEFT_CENTER;

  return container;
}

/* default expandable implementation */
static gboolean default_expandable(dt_lib_module_t *self)
{
  return TRUE;
}

/* default autoapply implementation */
static gboolean default_preset_autoapply(dt_lib_module_t *self)
{
  return FALSE;
}

static int dt_lib_load_module(void *m,
                              const char *libname,
                              const char *module_name)
{
  dt_lib_module_t *module = (dt_lib_module_t *)m;
  g_strlcpy(module->plugin_name, module_name, sizeof(module->plugin_name));

#define INCLUDE_API_FROM_MODULE_LOAD "lib_load_module"
#include "libs/lib_api.h"

  if(((!module->get_params || !module->set_params)
      && (module->legacy_params || module->set_params || module->get_params))
     || (!module->init_presets && module->manage_presets))
  {
    dt_print(DT_DEBUG_ALWAYS, "[dt_lib_load_module] illegal method combination in '%s'",
             module->plugin_name);
  }

  if(!module->get_params || !module->set_params)
  {
    // need all at the same time, or none, note that in this case
    // all the presets for the corresponding module will be deleted.
    // see: dt_lib_init_presets.
    module->legacy_params = NULL;
    module->set_params = NULL;
    module->get_params = NULL;
    module->manage_presets = NULL;
  }

  module->widget = NULL;
  module->expander = NULL;
  module->arrow = NULL;
  module->reset_button = NULL;
  module->presets_button = NULL;

  module->actions = (dt_action_t){ DT_ACTION_TYPE_LIB,
                                   module->plugin_name,
                                   module->name(module) };

  dt_action_insert_sorted(&darktable.control->actions_libs, &module->actions);
#ifdef USE_LUA
  dt_lua_lib_register(darktable.lua_state.state, module);
#endif

  if(module->init)
    module->init(module);

  return 0;
}

static void *_update_params(dt_lib_module_t *module,
                            const void *const old_params,
                            size_t old_params_size,
                            int old_version,
                            const int target_version,
                            size_t *new_size)
{
  // make a copy of the old params so we can free it in the loop
  void *params = malloc(old_params_size);
  if(params == NULL) return NULL;
  memcpy(params, old_params, old_params_size);
  while(old_version < target_version)
  {
    size_t size;
    int version;
    void *new_params = module->legacy_params(module, params, old_params_size,
                                             old_version, &version, &size);
    free(params);
    if(new_params == NULL) return NULL;
    params = new_params;
    old_version = version;
    old_params_size = size;
  }
  *new_size = old_params_size;
  return params;
}

void dt_lib_init_presets(dt_lib_module_t *module)
{
  // since lighttable presets can't end up in styles or any other
  // place outside of the presets table it is sufficient to update
  // that very table here and assume that everything is up to date
  // elsewhere.  the intended logic is as follows:
  // - no set_params -> delete all presets
  // - op_version >= module_version -> done
  // - op_version < module_version ->
  //   - module has legacy_params -> try to update
  //   - module doesn't have legacy_params -> delete it

  if(module->set_params == NULL)
  {
    sqlite3_stmt *stmt;
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM data.presets"
                                " WHERE operation=?1", -1,
                                &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->plugin_name, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  else
  {
    sqlite3_stmt *stmt;
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT rowid, op_version, op_params, name"
                                " FROM data.presets"
                                " WHERE operation=?1",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->plugin_name, -1, SQLITE_TRANSIENT);
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      int rowid = sqlite3_column_int(stmt, 0);
      int op_version = sqlite3_column_int(stmt, 1);
      void *op_params = (void *)sqlite3_column_blob(stmt, 2);
      size_t op_params_size = sqlite3_column_bytes(stmt, 2);
      const char *name = (char *)sqlite3_column_text(stmt, 3);

      int version = module->version();

      if(op_version < version)
      {
        size_t new_params_size = 0;
        void *new_params = NULL;

        if(module->legacy_params
          && (new_params = _update_params(module, op_params, op_params_size,
                                          op_version, version, &new_params_size)))
        {
          // write the updated preset back to db
          dt_print(DT_DEBUG_ALWAYS,
                   "[lighttable_init_presets] updating '%s' preset '%s'"
                   " from version %d to version %d",
                   module->plugin_name, name, op_version, version);
          sqlite3_stmt *innerstmt;
          // clang-format off
          DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                      "UPDATE data.presets"
                                      " SET op_version=?1, op_params=?2"
                                      " WHERE rowid=?3", -1,
                                      &innerstmt, NULL);
          // clang-format on
          DT_DEBUG_SQLITE3_BIND_INT(innerstmt, 1, version);
          DT_DEBUG_SQLITE3_BIND_BLOB(innerstmt, 2, new_params, new_params_size,
                                     SQLITE_TRANSIENT);
          DT_DEBUG_SQLITE3_BIND_INT(innerstmt, 3, rowid);
          sqlite3_step(innerstmt);
          sqlite3_finalize(innerstmt);
        }
        else
        {
          // delete the preset
          dt_print(DT_DEBUG_ALWAYS,
                   "[lighttable_init_presets] Can't upgrade '%s' preset '%s'"
                   " from version %d to %d, "
                   "no legacy_params() implemented or unable to update",
                   module->plugin_name, name, op_version, version);
          sqlite3_stmt *innerstmt;
          // clang-format off
          DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                      "DELETE FROM data.presets"
                                      " WHERE rowid=?1", -1,
                                      &innerstmt, NULL);
          // clang-format on
          DT_DEBUG_SQLITE3_BIND_INT(innerstmt, 1, rowid);
          sqlite3_step(innerstmt);
          sqlite3_finalize(innerstmt);
        }
        free(new_params);
      }
    }
    sqlite3_finalize(stmt);
  }

  if(module->init_presets)
    module->init_presets(module);

  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_PRESETS_CHANGED,
                          g_strdup(module->plugin_name));

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name"
                              " FROM data.presets"
                              " WHERE operation=?1 AND op_version=?2"
                              " ORDER BY writeprotect DESC, name, rowid",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_action_define_preset(&module->actions, (char *)sqlite3_column_text(stmt, 0));
  }
  sqlite3_finalize(stmt);
}

static gboolean _lib_draw_callback(GtkWidget *widget,
                                   gpointer cr,
                                   dt_lib_module_t *self)
{
  dt_lib_gui_update(self);

  return FALSE;
}

void dt_lib_gui_queue_update(dt_lib_module_t *module)
{
  module->gui_uptodate = FALSE;
  gtk_widget_queue_draw(module->widget);
}

void dt_lib_gui_update(dt_lib_module_t *module)
{
  if(module && module->gui_update && !module->gui_uptodate)
  {
    module->gui_update(module);
    module->gui_uptodate = TRUE;
  }
}

static void dt_lib_init_module(void *m)
{
  dt_lib_module_t *module = (dt_lib_module_t *)m;
  dt_lib_init_presets(module);

  // Calling the keyboard shortcut initialization callback if present
  // do not init accelerators if there is no gui
  if(darktable.gui)
  {
    module->gui_init(module);

    if(module->widget)
    {
      g_object_ref_sink(module->widget);
      if(module->gui_update)
        g_signal_connect(G_OBJECT(module->widget), "draw",
                         G_CALLBACK(_lib_draw_callback), module);
    }
  }
}

void dt_lib_unload_module(dt_lib_module_t *module)
{
  if(module->module)
    g_module_close(module->module);
}

static void dt_lib_gui_reset_callback(GtkButton *button,
                                      gpointer user_data)
{
  dt_lib_module_t *module = (dt_lib_module_t *)user_data;
  module->gui_reset(module);
}

static void presets_popup_callback(GtkButton *button,
                                   dt_lib_module_t *module)
{
  dt_lib_module_info_t *mi = calloc(1, sizeof(dt_lib_module_info_t));

  mi->plugin_name = g_strdup(module->plugin_name);
  mi->version = module->version();
  mi->module = module;
  mi->params = module->get_params ? module->get_params(module, &mi->params_size) : NULL;
  if(!mi->params)
  {
    // this is a valid case, for example in location.c when nothing got selected
    // fprintf(stderr, "something went wrong: &params=%p, size=%i\n",
    //         mi->params, mi->params_size);
    mi->params_size = 0;
  }
  dt_lib_presets_popup_menu_show(mi, GTK_WIDGET(button));

  if(button)
    dtgtk_button_set_active(DTGTK_BUTTON(button), FALSE);
}


void dt_lib_gui_set_expanded(dt_lib_module_t *module, gboolean expanded)
{
  if(!module->expander || !module->arrow) return;

  dtgtk_expander_set_expanded(DTGTK_EXPANDER(module->expander), expanded);

  /* update expander arrow state */
  gint flags = (expanded ? CPF_DIRECTION_DOWN : CPF_DIRECTION_RIGHT);
  dtgtk_button_set_paint(DTGTK_BUTTON(module->arrow), dtgtk_cairo_paint_solid_arrow,
                         flags, NULL);

  darktable.lib->gui_module = expanded ? module : NULL;

  /* store expanded state of module */
  char var[1024];
  const dt_view_t *current_view = dt_view_manager_get_current_view(darktable.view_manager);
  snprintf(var, sizeof(var), "plugins/%s/%s/expanded",
           current_view->module_name, module->plugin_name);
  dt_conf_set_bool(var, expanded);
}

gboolean dt_lib_gui_get_expanded(dt_lib_module_t *module)
{
  if(!module->expandable(module)) return true;
  if(!module->expander) return true;
  if(!module->widget)
  {
    char var[1024];
    const dt_view_t *current_view =
      dt_view_manager_get_current_view(darktable.view_manager);
    snprintf(var, sizeof(var), "plugins/%s/%s/expanded",
             current_view->module_name, module->plugin_name);
    return dt_conf_get_bool(var);
  }
  return dtgtk_expander_get_expanded(DTGTK_EXPANDER(module->expander));
}

static gboolean _lib_plugin_header_button_press(GtkWidget *w,
                                                GdkEventButton *e,
                                                gpointer user_data)
{
  if(e->type == GDK_2BUTTON_PRESS || e->type == GDK_3BUTTON_PRESS) return TRUE;

  dt_lib_module_t *module = (dt_lib_module_t *)user_data;

  if(e->button == 1)
  {
    /* bail out if module is static */
    if(!module->expandable(module)) return FALSE;

    if(dt_modifier_is(e->state, GDK_SHIFT_MASK | GDK_CONTROL_MASK))
      ; // do nothing (for easier dragging)
    /* handle shiftclick on expander, hide all except this */
    else if(!dt_conf_get_bool("lighttable/ui/single_module") !=
            !dt_modifier_is(e->state, GDK_SHIFT_MASK))
    {
      const dt_view_t *v = dt_view_manager_get_current_view(darktable.view_manager);
      gboolean all_other_closed = TRUE;
      for(const GList *it = darktable.lib->plugins; it; it = g_list_next(it))
      {
        dt_lib_module_t *m = it->data;

        if(m != module
           && module->container(module) == m->container(m)
           && m->expandable(m) && dt_lib_is_visible_in_view(m, v))
        {
          all_other_closed = all_other_closed
            && !dtgtk_expander_get_expanded(DTGTK_EXPANDER(m->expander));
          dt_lib_gui_set_expanded(m, FALSE);
        }
      }
      if(all_other_closed)
        dt_lib_gui_set_expanded
          (module,
           !dtgtk_expander_get_expanded(DTGTK_EXPANDER(module->expander)));
      else
        dt_lib_gui_set_expanded(module, TRUE);
    }
    else
    {
      /* else just toggle */
      dt_lib_gui_set_expanded
        (module,
         !dtgtk_expander_get_expanded(DTGTK_EXPANDER(module->expander)));
    }

    //ensure that any gtkentry fields lose focus
    gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

    return TRUE;
  }
  else if(e->button == 3)
  {
    if(gtk_widget_get_sensitive(module->presets_button))
      presets_popup_callback(NULL, module);

    return TRUE;
  }
  return FALSE;
}

static void show_module_callback(dt_lib_module_t *module)
{
  /* bail out if module is static */
  if(!module->expandable(module)) return;

  if(dt_conf_get_bool("lighttable/ui/single_module"))
  {
    const dt_view_t *v = dt_view_manager_get_current_view(darktable.view_manager);
    gboolean all_other_closed = TRUE;
    for(const GList *it = darktable.lib->plugins; it; it = g_list_next(it))
    {
      dt_lib_module_t *m = it->data;

      if(m != module
         && module->container(module) == m->container(m)
         && m->expandable(m)
         && dt_lib_is_visible_in_view(m, v))
      {
        all_other_closed = all_other_closed
          && !dtgtk_expander_get_expanded(DTGTK_EXPANDER(m->expander));
        dt_lib_gui_set_expanded(m, FALSE);
      }
    }
    if(all_other_closed)
      dt_lib_gui_set_expanded
        (module, !dtgtk_expander_get_expanded(DTGTK_EXPANDER(module->expander)));
    else
      dt_lib_gui_set_expanded(module, TRUE);
  }
  else
  {
    /* else just toggle */
    dt_lib_gui_set_expanded
      (module, !dtgtk_expander_get_expanded(DTGTK_EXPANDER(module->expander)));
  }
}

static gboolean _header_enter_notify_callback(GtkWidget *eventbox,
                                              GdkEventCrossing *event,
                                              gpointer user_data)
{
  darktable.control->element = GPOINTER_TO_INT(user_data);
  return FALSE;
}

static gboolean _body_enter_leave_callback(GtkWidget *widget,
                                            GdkEventCrossing *e,
                                            gpointer user_data)
{
  // set or clear focused module when entering or leaving (not when opening popup)
  if(e->detail != GDK_NOTIFY_INFERIOR && e->mode == GDK_CROSSING_NORMAL)
    darktable.lib->gui_module = e->type == GDK_ENTER_NOTIFY ? user_data : NULL;

  return FALSE;
}

static gboolean _on_drag_motion(GtkWidget *widget,
                                GdkDragContext *dc,
                                gint x, gint y, guint time,
                                dt_lib_module_t *dest)
{
  dt_lib_module_t *src = NULL;
  GtkContainer *src_panel = NULL, *dest_panel = NULL;
  gboolean below = TRUE;
  int dest_pos = 0;

  if(GTK_IS_BOX(dc))
  {
    // propagated from empty panel handler in gtk.c
    src = dest;
    src_panel = GTK_CONTAINER(gtk_widget_get_parent(src->expander));
    dest_panel = GTK_CONTAINER(dc);
  }
  else
  {
    dtgtk_expander_set_drag_hover(DTGTK_EXPANDER(widget), FALSE, TRUE, time);
    gdk_drag_status(dc, 0, time);

    GtkWidget *src_header = gtk_drag_get_source_widget(dc);
    if(!src_header) return TRUE;
    GtkWidget *src_expander = gtk_widget_get_ancestor(src_header, DTGTK_TYPE_EXPANDER);

    for(GList *lib = darktable.lib->plugins; lib; lib = lib->next)
      if(((dt_lib_module_t *)lib->data)->expander == src_expander)
        src = lib->data;

    if(!src_expander || !src || dest == src ) return TRUE;

    src_panel = GTK_CONTAINER(gtk_widget_get_parent(src->expander));
    dest_panel = GTK_CONTAINER(gtk_widget_get_parent(dest->expander));

    int src_pos = G_MAXINT;
    if(dest_panel == src_panel)
      gtk_container_child_get(src_panel, src->expander, "position", &src_pos, NULL);
    gtk_container_child_get(dest_panel, dest->expander, "position", &dest_pos, NULL);

    GtkDarktableExpander *exp = DTGTK_EXPANDER(dest->expander);
    int header_height = gtk_widget_get_allocated_height(dtgtk_expander_get_header(exp));
    below = y > (ABS(dest_pos - src_pos) == 1 && !dtgtk_expander_get_expanded(exp)
                 ? header_height / 2
                 : dest_pos < src_pos
                 ? gtk_widget_get_allocated_height(widget) - header_height
                 : header_height);

    if(below) dest_pos++;
    if(dest_pos > src_pos) dest_pos--;
    if(dest_pos == src_pos) return TRUE;

    if(x != DND_DROP)
    {
      dtgtk_expander_set_drag_hover(DTGTK_EXPANDER(widget), TRUE, below, time);
      gdk_drag_status(dc, GDK_ACTION_COPY, time);

      return TRUE;
    }
  }

  GtkBox *dest_box = GTK_BOX(dest_panel);
  if(dest_panel != src_panel)
  {
    g_object_ref(src->expander);
    gtk_container_remove(src_panel, src->expander);
    gtk_box_pack_start(dest_box, src->expander, FALSE, FALSE, 0);
  }
  gtk_box_reorder_child(dest_box, src->expander, dest_pos);

  GList **list = &darktable.lib->plugins;
  *list = g_list_remove(*list, src);

  GList *dest_list = g_list_find(*list, dest);
  if(below)
    dest = dest_list && dest_list->prev ? dest_list->prev->data : NULL;
  else
    dest_list = dest_list->next;

  int new_pos = dest ? ABS(_lib_position(dest)) + 1 : 1;
  int old_pos = dest_box != dt_ui_get_container(darktable.gui->ui, src->container(src)) ? -1 : +1;
  dt_lib_module_t *cur = src;
  while(new_pos >= ABS(old_pos))
  {
    gchar *key = _get_lib_view_path(cur, NULL, "_position");
    dt_conf_set_int(key, old_pos < 0 ? -new_pos : new_pos);
    g_free(key);

    if(!dest_list) break;

    new_pos++;
    cur = dest_list->data;
    old_pos = _lib_position(cur);
    dest_list = dest_list->next;
  }

  *list = g_list_insert_sorted(*list, src, dt_lib_sort_plugins);

  return TRUE;
}

static gboolean _on_drag_drop(GtkWidget *widget,
                              GdkDragContext *dc,
                              gint x, gint y, guint time,
                              dt_lib_module_t *dest)
{
  return _on_drag_motion(widget, dc, DND_DROP, y, time, dest);
}

GtkWidget *dt_lib_gui_get_expander(dt_lib_module_t *module)
{
  /* check if module is expandable */
  if(!module->expandable(module))
  {
    if(module->presets_button)
    {
      // FIXME separately define as darkroom widget shortcut/action,
      // because not automatically registered via lib if presets btn
      // has been loaded to be shown outside expander
      g_signal_connect(G_OBJECT(module->presets_button), "clicked",
                       G_CALLBACK(presets_popup_callback), module);
    }
    module->expander = NULL;
    return NULL;
  }

  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(GTK_WIDGET(header), "module-header");

  GtkWidget *expander = dtgtk_expander_new(header, module->widget);
  GtkWidget *header_evb = dtgtk_expander_get_header_event_box(DTGTK_EXPANDER(expander));
  GtkWidget *body_evb = dtgtk_expander_get_body_event_box(DTGTK_EXPANDER(expander));
  GtkWidget *pluginui_frame = dtgtk_expander_get_frame(DTGTK_EXPANDER(expander));

  dt_ui_container_t container = module->container(module);
  if(container == DT_UI_CONTAINER_PANEL_LEFT_CENTER || container == DT_UI_CONTAINER_PANEL_RIGHT_CENTER)
  {
    static const GtkTargetEntry target_list[] = { { "lib", GTK_TARGET_SAME_APP, DND_TARGET_LIB } };

    gtk_drag_source_set(header_evb, GDK_BUTTON1_MASK, target_list, 1, GDK_ACTION_COPY);
    gtk_drag_dest_set(expander, GTK_DEST_DEFAULT_DROP | GTK_DEST_DEFAULT_HIGHLIGHT, target_list, 1, GDK_ACTION_COPY);
    g_signal_connect(expander, "drag-motion", G_CALLBACK(_on_drag_motion), module);
    g_signal_connect(expander, "drag-drop", G_CALLBACK(_on_drag_drop), module);
  }

  /* setup the header box */
  g_signal_connect(G_OBJECT(header_evb), "button-press-event",
                   G_CALLBACK(_lib_plugin_header_button_press),
                   module);
  g_signal_connect(G_OBJECT(header_evb), "enter-notify-event",
                   G_CALLBACK(_header_enter_notify_callback),
                   GINT_TO_POINTER(DT_ACTION_ELEMENT_SHOW));

  /* (un)focus module when entering/leaving body */
  g_signal_connect(G_OBJECT(body_evb), "enter-notify-event",
                   G_CALLBACK(_body_enter_leave_callback), module);
  g_signal_connect(G_OBJECT(body_evb), "leave-notify-event",
                   G_CALLBACK(_body_enter_leave_callback), module);

  /*
   * initialize the header widgets
   */
  /* add the expand indicator icon */
  module->arrow = dtgtk_button_new(dtgtk_cairo_paint_solid_arrow, 0, NULL);

  gtk_widget_set_tooltip_text(module->arrow, _("show module"));
  g_signal_connect(G_OBJECT(module->arrow), "button-press-event",
                    G_CALLBACK(_lib_plugin_header_button_press), module);
  dt_action_define(&module->actions, NULL, NULL, module->arrow, NULL);
  gtk_box_pack_start(GTK_BOX(header), module->arrow, FALSE, FALSE, 0);

  /* add module label */
  GtkWidget *label = gtk_label_new("");
  GtkWidget *label_evb = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(label_evb), label);
  gchar *mname = g_markup_escape_text(module->name(module), -1);
  gtk_label_set_markup(GTK_LABEL(label), mname);
  if(module->description)
    gtk_widget_set_tooltip_text(header, module->description(module));
  else
    gtk_widget_set_tooltip_text(header, mname);
  g_free(mname);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  g_object_set(G_OBJECT(label), "halign", GTK_ALIGN_START, "xalign", 0.0, (gchar *)0);
  gtk_widget_set_name(label, "lib-panel-label");
  dt_action_define(&module->actions, NULL, NULL, label_evb, NULL);
  gtk_box_pack_start(GTK_BOX(header), label_evb, FALSE, FALSE, 0);

  /* add preset button if module has implementation */
  module->presets_button = dtgtk_button_new(dtgtk_cairo_paint_presets, 0, NULL);
  gtk_widget_set_tooltip_text(module->presets_button, _("presets and preferences"));
  g_signal_connect(G_OBJECT(module->presets_button), "clicked",
                   G_CALLBACK(presets_popup_callback), module);
  g_signal_connect(G_OBJECT(module->presets_button), "enter-notify-event",
                   G_CALLBACK(_header_enter_notify_callback),
                   GINT_TO_POINTER(DT_ACTION_ELEMENT_PRESETS));
  if(!module->get_params
     && !module->set_preferences)
    gtk_widget_set_sensitive(GTK_WIDGET(module->presets_button), FALSE);

  dt_action_define(&module->actions, NULL, NULL, module->presets_button, NULL);
  gtk_box_pack_end(GTK_BOX(header), module->presets_button, FALSE, FALSE, 0);

  /* add reset button if module has implementation */
  module->reset_button = dtgtk_button_new(dtgtk_cairo_paint_reset, 0, NULL);
  g_signal_connect(G_OBJECT(module->reset_button), "clicked",
                   G_CALLBACK(dt_lib_gui_reset_callback), module);
  g_signal_connect(G_OBJECT(module->reset_button), "enter-notify-event",
                   G_CALLBACK(_header_enter_notify_callback),
                   GINT_TO_POINTER(DT_ACTION_ELEMENT_RESET));
  if(!module->gui_reset) gtk_widget_set_sensitive(module->reset_button, FALSE);
  dt_action_define(&module->actions, NULL, NULL, module->reset_button, NULL);
  gtk_box_pack_end(GTK_BOX(header), module->reset_button, FALSE, FALSE, 0);

  /* add button box - for module's specific action button */
  if(module->gui_tool_box)
    gtk_box_pack_end(GTK_BOX(header), module->gui_tool_box(module), FALSE, FALSE, 0);

  gtk_widget_show_all(expander);

  if(module->widget)
  {
    dt_gui_add_class(module->widget, "dt_plugin_ui_main");
    gtk_widget_set_hexpand(module->widget, FALSE);
    gtk_widget_set_vexpand(module->widget, FALSE);
  }
  dt_gui_add_class(pluginui_frame, "dt_plugin_ui");
  module->expander = expander;


  return module->expander;
}

void dt_lib_gui_set_label(dt_lib_module_t *module,
                          const char *label)
{
  if(!module->expander) return;
  GtkWidget *header = DTGTK_EXPANDER(module->expander)->header;
  gtk_box_set_center_widget(GTK_BOX(header), gtk_label_new(label));
  gtk_widget_show_all(header);
}

static void _preferences_changed(gpointer instance, gpointer self)
{
  // reload presets if they are based on the actual workflow which
  // could have been changed after editing the preferences.

  dt_lib_t *lib = (dt_lib_t *)self;

  GList *p = lib->plugins;

  while(p)
  {
    dt_lib_module_t *lmod = p->data;

    if(lmod->pref_based_presets)
      dt_lib_init_presets(lmod);

    p = g_list_next(p);
  }
}

void dt_lib_init(dt_lib_t *lib)
{
  // Setting everything to null initially
  memset(lib, 0, sizeof(dt_lib_t));
  darktable.lib->plugins = dt_module_load_modules("/plugins/lighttable",
                                                  sizeof(dt_lib_module_t),
                                                  dt_lib_load_module,
                                                  dt_lib_init_module,
                                                  dt_lib_sort_plugins);
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_PREFERENCES_CHANGE, _preferences_changed, lib);
}

void dt_lib_cleanup(dt_lib_t *lib)
{
  DT_CONTROL_SIGNAL_DISCONNECT(_preferences_changed, lib);
  while(lib->plugins)
  {
    dt_lib_module_t *module = lib->plugins->data;
    if(module)
    {
      if(module->data != NULL)
      {
        module->gui_cleanup(module);
        module->data = NULL;
      }
      dt_lib_unload_module(module);
      free(module);
    }
    lib->plugins = g_list_delete_link(lib->plugins, lib->plugins);
  }
}

void dt_lib_presets_add(const char *name,
                        const char *plugin_name,
                        const int32_t version,
                        const void *params,
                        const int32_t params_size,
                        const gboolean readonly,
                        const dt_gui_presets_format_flag_t format)
{
  dt_lib_presets_remove(name, plugin_name, version);

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "INSERT OR REPLACE INTO data.presets"
      " (name, description, operation, op_version, op_params, "
      "  blendop_params, blendop_version, enabled, model, maker, lens, "
      "  iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max, "
      "  focal_length_min, focal_length_max, writeprotect, "
      "  autoapply, filter, def, format)"
      " VALUES "
      "  (?1, '', ?2, ?3, ?4, NULL, 0, 1, '%', "
      "   '%', '%', 0, 340282346638528859812000000000000000000, 0, 10000000,"
      "   0, 100000000, 0, 1000, ?5, ?6, 0, 0, ?7)",
      -1, &stmt, NULL);
  // clang-format on

  const int autoapply = format == 0 ? 0 : 1;

  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, readonly);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 6, autoapply);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, format);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static gchar *_get_lib_view_path(const dt_lib_module_t *module,
                                 const dt_view_t *cv,
                                 char *suffix)
{
  if(!cv && darktable.view_manager)
    cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(!cv) return NULL;
  // in lighttable, we store panels states per layout
  char lay[32] = "";
  if(g_strcmp0(cv->module_name, "lighttable") == 0 && !module->expandable((gpointer)module))
  {
    if(dt_view_lighttable_preview_state(darktable.view_manager))
      g_snprintf(lay, sizeof(lay), "preview/");
    else
      g_snprintf(lay, sizeof(lay), "%d/",
                 dt_view_lighttable_get_layout(darktable.view_manager));
  }
  else if(g_strcmp0(cv->module_name, "darkroom") == 0)
  {
    g_snprintf(lay, sizeof(lay), "%d/",
               dt_view_darkroom_get_layout(darktable.view_manager));
  }

  return g_strdup_printf("plugins/%s/%s%s%s", cv->module_name,
                         lay, module->plugin_name, suffix);
}

gboolean dt_lib_is_visible(dt_lib_module_t *module)
{
  return dt_lib_is_visible_in_view(module, dt_view_manager_get_current_view(darktable.view_manager));
}

void dt_lib_set_visible(dt_lib_module_t *module,
                        const gboolean visible)
{
  gchar *key = _get_lib_view_path(module, NULL, "_visible");
  GtkWidget *widget;
  if(key)
    dt_conf_set_bool(key, visible);
  g_free(key);

  if(module->widget)
  {
    if(module->expander)
      widget = module->expander;
    else
      widget = module->widget;

    if(visible)
      gtk_widget_show(GTK_WIDGET(widget));
    else
      gtk_widget_hide(GTK_WIDGET(widget));
  }
}

gchar *dt_lib_get_localized_name(const gchar *plugin_name)
{
  // Prepare mapping op -> localized name
  static GHashTable *module_names = NULL;
  if(module_names == NULL)
  {
    module_names = g_hash_table_new(g_str_hash, g_str_equal);
    for(const GList *lib = darktable.lib->plugins; lib; lib = g_list_next(lib))
    {
      dt_lib_module_t *module = lib->data;
      g_hash_table_insert(module_names, module->plugin_name,
                          g_strdup(module->name(module)));
    }
  }

  return (gchar *)g_hash_table_lookup(module_names, plugin_name);
}

void dt_lib_colorpicker_set_box_area(dt_lib_t *lib,
                                     const dt_pickerbox_t box)
{
  if(!lib->proxy.colorpicker.module || !lib->proxy.colorpicker.set_sample_box_area) return;
  lib->proxy.colorpicker.set_sample_box_area(lib->proxy.colorpicker.module, box);
  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
}

void dt_lib_colorpicker_set_point(dt_lib_t *lib,
                                  const float pos[2])
{
  if(!lib->proxy.colorpicker.module || !lib->proxy.colorpicker.set_sample_point) return;
  lib->proxy.colorpicker.set_sample_point(lib->proxy.colorpicker.module, pos);
  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
}

void dt_lib_colorpicker_setup(dt_lib_t *lib,
                              const gboolean denoise,
                              const gboolean pick_output)

{
  if(!lib->proxy.colorpicker.module || !lib->proxy.colorpicker.setup_sample) return;
  lib->proxy.colorpicker.setup_sample(lib->proxy.colorpicker.module, denoise, pick_output);
}

dt_lib_module_t *dt_lib_get_module(const char *name)
{
  /* hide/show modules as last config */
  for(GList *iter = darktable.lib->plugins; iter; iter = g_list_next(iter))
  {
    dt_lib_module_t *plugin = iter->data;
    if(strcmp(plugin->plugin_name, name) == 0)
      return plugin;
  }

  return NULL;
}

gboolean dt_lib_presets_can_autoapply(dt_lib_module_t *mod)
{
  return mod->preset_autoapply(mod);
}

static float _action_process(gpointer target,
                             const dt_action_element_t element,
                             const dt_action_effect_t effect,
                             const float move_size)
{
  dt_lib_module_t *module = target;

  if(DT_PERFORM_ACTION(move_size))
  {
    switch(element)
    {
    case DT_ACTION_ELEMENT_SHOW:
      show_module_callback(module);
      break;
    case DT_ACTION_ELEMENT_RESET:
      if(module->gui_reset) dt_lib_gui_reset_callback(NULL, module);
      break;
    case DT_ACTION_ELEMENT_PRESETS:
      if(module->get_params || module->set_preferences)
        presets_popup_callback(NULL, module);
      break;
    }
  }

  return element == DT_ACTION_ELEMENT_SHOW
    && dtgtk_expander_get_expanded(DTGTK_EXPANDER(module->expander));
}

static const dt_action_element_def_t _action_elements[]
  = { { N_("show"), dt_action_effect_toggle },
      { N_("reset"), dt_action_effect_activate },
      { N_("presets"), dt_action_effect_presets },
      { NULL } };

static const dt_shortcut_fallback_t _action_fallbacks[]
  = { { .element = DT_ACTION_ELEMENT_SHOW,
        .button = DT_SHORTCUT_LEFT },
      { .element = DT_ACTION_ELEMENT_RESET,
        .button = DT_SHORTCUT_LEFT,
        .click = DT_SHORTCUT_DOUBLE },
      { .element = DT_ACTION_ELEMENT_PRESETS,
        .button = DT_SHORTCUT_RIGHT },
      { } };

const dt_action_def_t dt_action_def_lib
  = { N_("utility module"),
      _action_process,
      _action_elements,
      _action_fallbacks };

gboolean dt_handle_dialog_enter(GtkWidget *widget,
                                GdkEventKey *event,
                                gpointer data)
{
  if(event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter)
  {
    gtk_dialog_response(GTK_DIALOG(widget), GTK_RESPONSE_ACCEPT);
    return TRUE;
  }
  return FALSE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
