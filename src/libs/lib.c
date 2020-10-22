/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

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
#include "gui/gtk.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <stdbool.h>
#include <stdlib.h>

typedef struct dt_lib_module_info_t
{
  char *plugin_name;
  int32_t version;
  char *params;
  int params_size;
  dt_lib_module_t *module;
} dt_lib_module_info_t;

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

typedef enum dt_module_header_icons_t
{
  DT_MODULE_ARROW = 0,
  DT_MODULE_LABEL,
  DT_MODULE_RESET,
  DT_MODULE_PRESETS,
  DT_MODULE_LAST
} dt_module_header_icons_t;


gboolean dt_lib_is_visible_in_view(dt_lib_module_t *module, const dt_view_t *view)
{
  if(!module->views)
  {
    fprintf(stderr, "module %s doesn't have views flags\n", module->name(module));
    return FALSE;
  }

  const char **views = module->views(module);
  for(const char **iter = views; *iter; iter++)
  {
    if(!strcmp(*iter, "*") || !strcmp(*iter, view->module_name)) return TRUE;
  }
  return FALSE;
}

/** calls module->cleanup and closes the dl connection. */
static void dt_lib_unload_module(dt_lib_module_t *module);

static gchar *get_active_preset_name(dt_lib_module_info_t *minfo)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT name, op_params, writeprotect"
      " FROM data.presets"
      " WHERE operation=?1 AND op_version=?2",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, minfo->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, minfo->version);
  gchar *name = NULL;
  // collect all presets for op from db
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    void *op_params = (void *)sqlite3_column_blob(stmt, 1);
    int32_t op_params_size = sqlite3_column_bytes(stmt, 1);
    if(op_params_size == minfo->params_size && !memcmp(minfo->params, op_params, op_params_size))
    {
      name = g_strdup((char *)sqlite3_column_text(stmt, 0));
      break;
    }
  }
  sqlite3_finalize(stmt);
  return name;
}

static void edit_preset_response(GtkDialog *dialog, gint response_id, dt_lib_presets_edit_dialog_t *g)
{
  gint dlg_ret;
  gint is_new = 0;

  if(response_id == GTK_RESPONSE_ACCEPT)
  {
    sqlite3_stmt *stmt;

    // now delete preset, so we can re-insert the new values:
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM data.presets"
                                " WHERE name=?1 AND operation=?2 AND op_version=?3",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, g->original_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, g->plugin_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, g->version);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    const char *name = gtk_entry_get_text(g->name);
    if(((g->old_id >= 0) && (strcmp(g->original_name, name) != 0)) || (g->old_id < 0))
    {

      // editing existing preset with different name or store new preset -> check for a preset with the same
      // name:

      DT_DEBUG_SQLITE3_PREPARE_V2(
          dt_database_get(darktable.db),
          "SELECT name"
          " FROM data.presets"
          " WHERE name = ?1 AND operation=?2 AND op_version=?3", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, g->plugin_name, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, g->version);

      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        sqlite3_finalize(stmt);

        GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
        GtkWidget *dlg_overwrite = gtk_message_dialog_new(
            GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO,
            _("preset `%s' already exists.\ndo you want to overwrite?"), name);
#ifdef GDK_WINDOWING_QUARTZ
        dt_osx_disallow_fullscreen(dlg_overwrite);
#endif
        gtk_window_set_title(GTK_WINDOW(dlg_overwrite), _("overwrite preset?"));
        dlg_ret = gtk_dialog_run(GTK_DIALOG(dlg_overwrite));
        gtk_widget_destroy(dlg_overwrite);

        // if result is BUTTON_NO exit without destroy dialog, to permit other name
        if(dlg_ret == GTK_RESPONSE_NO) return;
      }
      else
      {
        is_new = 1;
        sqlite3_finalize(stmt);
      }
    }

    if(is_new == 0)
    {
      // delete preset, so we can re-insert the new values:
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "DELETE FROM data.presets"
                                  " WHERE name=?1 AND operation=?2 AND op_version=?3", -1,
                                  &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, g->plugin_name, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, g->version);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }


    // commit all the user input fields
    dt_accel_rename_preset_lib(g->module, g->original_name, name);
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get(darktable.db),
       "INSERT INTO data.presets (name, description, operation, op_version, op_params,"
       "  blendop_params, blendop_version, enabled, model, maker, lens,"
       "  iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max,"
       "  focal_length_min, focal_length_max, writeprotect,"
       "  autoapply, filter, def, format)"
       " VALUES (?1, ?2, ?3, ?4, ?5, NULL, 0, 1, "
       "         '%', '%', '%', 0, 340282346638528859812000000000000000000, 0, 100000000, 0, "
       "          100000000, 0, 1000, 0, 0, 0, 0, 0)",
       -1, &stmt, NULL);

    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, gtk_entry_get_text(g->description), -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, g->plugin_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, g->version);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 5, g->params, g->params_size, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    dt_gui_store_last_preset(name);
  }
  gtk_widget_destroy(GTK_WIDGET(dialog));
  g_free(g->original_name);
  free(g);
}

static void edit_preset(const char *name_in, dt_lib_module_info_t *minfo)
{
  gchar *name = NULL;
  if(name_in == NULL)
  {
    name = get_active_preset_name(minfo);
    if(name == NULL) return;
  }
  else
    name = g_strdup(name_in);

  GtkWidget *dialog;
  /* Create the widgets */
  char title[1024];
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  snprintf(title, sizeof(title), _("edit `%s'"), name);
  dialog = gtk_dialog_new_with_buttons(title, GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT,
                                       _("_cancel"), GTK_RESPONSE_REJECT, _("_ok"), GTK_RESPONSE_ACCEPT, NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  GtkContainer *content_area = GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog)));
  GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_container_add(content_area, GTK_WIDGET(box));

  dt_lib_presets_edit_dialog_t *g
      = (dt_lib_presets_edit_dialog_t *)g_malloc0(sizeof(dt_lib_presets_edit_dialog_t));
  g->old_id = -1;
  g_strlcpy(g->plugin_name, minfo->plugin_name, sizeof(g->plugin_name));
  g->version = minfo->version;
  g->params_size = minfo->params_size;
  g->params = minfo->params;
  g->name = GTK_ENTRY(gtk_entry_new());
  g->module = minfo->module;
  g->original_name = name;
  gtk_entry_set_text(g->name, name);
  gtk_box_pack_start(box, GTK_WIDGET(g->name), FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->name), _("name of the preset"));

  g->description = GTK_ENTRY(gtk_entry_new());
  gtk_box_pack_start(box, GTK_WIDGET(g->description), FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->description), _("description or further information"));

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT rowid, description"
      " FROM data.presets"
      " WHERE name = ?1 AND operation = ?2 AND op_version = ?3",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, minfo->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, minfo->version);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    g->old_id = sqlite3_column_int(stmt, 0);
    gtk_entry_set_text(g->description, (const char *)sqlite3_column_text(stmt, 1));
  }
  sqlite3_finalize(stmt);

  g_signal_connect(dialog, "response", G_CALLBACK(edit_preset_response), g);
  gtk_widget_show_all(dialog);
}

static void menuitem_update_preset(GtkMenuItem *menuitem, dt_lib_module_info_t *minfo)
{
  char *name = g_object_get_data(G_OBJECT(menuitem), "dt-preset-name");

  gint res = GTK_RESPONSE_YES;

  if(dt_conf_get_bool("plugins/lighttable/preset/ask_before_delete_preset"))
  {
    GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
    GtkWidget *dialog
      = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
                               GTK_BUTTONS_YES_NO, _("do you really want to update the preset `%s'?"), name);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif
    gtk_window_set_title(GTK_WINDOW(dialog), _("update preset?"));
    res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }

  if(res == GTK_RESPONSE_YES)
  {
    // commit all the module fields
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE data.presets"
                                " SET op_version=?2, op_params=?3"
                                " WHERE name=?4 AND operation=?1",
                                -1, &stmt, NULL);

    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, minfo->plugin_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, minfo->version);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, minfo->params, minfo->params_size, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

static void menuitem_new_preset(GtkMenuItem *menuitem, dt_lib_module_info_t *minfo)
{
  // add new preset
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM data.presets"
                              " WHERE name=?1 AND operation=?2 AND op_version=?3", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, _("new preset"), -1, SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, minfo->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, minfo->version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "INSERT INTO data.presets (name, description, operation, op_version, op_params,"
      "  blendop_params, blendop_version, enabled, model, maker, lens,"
      "  iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max,"
      "  focal_length_min, focal_length_max, writeprotect, "
      "  autoapply, filter, def, format)"
      " VALUES (?1, '', ?2, ?3, ?4, NULL, 0, 1, '%', "
      "         '%', '%', 0, 340282346638528859812000000000000000000, 0, 100000000, 0, 100000000,"
      "          0, 1000, 0, 0, 0, 0, 0)",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, _("new preset"), -1, SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, minfo->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, minfo->version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, minfo->params, minfo->params_size, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  // create a shortcut for the new entry
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", _("preset"), _("new preset"));
  dt_accel_register_lib(minfo->module, path, 0, 0);
  dt_accel_connect_preset_lib(minfo->module, _("new preset"));
  // then show edit dialog
  edit_preset(_("new preset"), minfo);
}

static void menuitem_edit_preset(GtkMenuItem *menuitem, dt_lib_module_info_t *minfo)
{
  edit_preset(NULL, minfo);
}

static void menuitem_manage_presets(GtkMenuItem *menuitem, dt_lib_module_info_t *minfo)
{
  if(minfo->module->manage_presets) minfo->module->manage_presets(minfo->module);
}

static void menuitem_delete_preset(GtkMenuItem *menuitem, dt_lib_module_info_t *minfo)
{
  sqlite3_stmt *stmt;
  gchar *name = get_active_preset_name(minfo);
  if(name == NULL) return;

  gint res = GTK_RESPONSE_YES;

  if(dt_conf_get_bool("plugins/lighttable/preset/ask_before_delete_preset"))
  {
    GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
    GtkWidget *dialog
      = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
                               GTK_BUTTONS_YES_NO, _("do you really want to delete the preset `%s'?"), name);
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
#endif
    gtk_window_set_title(GTK_WINDOW(dialog), _("delete preset?"));
    res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }

  if(res == GTK_RESPONSE_YES)
  {
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s", _("preset"), name);
    dt_accel_deregister_lib(minfo->module, tmp_path);
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get(darktable.db),
        "DELETE FROM data.presets"
        " WHERE name=?1 AND operation=?2 AND op_version=?3 AND writeprotect=0",
        -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, minfo->plugin_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, minfo->version);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  g_free(name);
}

gchar *dt_lib_presets_duplicate(gchar *preset, gchar *module_name, int module_version)
{
  sqlite3_stmt *stmt;

  // find the new name
  int i = 0;
  gboolean ko = TRUE;
  while(ko)
  {
    i++;
    gchar *tx = dt_util_dstrcat(NULL, "%s_%d", preset, i);
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get(darktable.db),
        "SELECT name"
        " FROM data.presets"
        " WHERE operation = ?1 AND op_version = ?2 AND name = ?3", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module_version);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, tx, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(stmt) != SQLITE_ROW) ko = FALSE;
    sqlite3_finalize(stmt);
    g_free(tx);
  }
  gchar *nname = dt_util_dstrcat(NULL, "%s_%d", preset, i);

  // and we duplicate the entry
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
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, nname, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, module_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, module_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, preset, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return nname;
}

void dt_lib_presets_remove(gchar *preset, gchar *module_name, int module_version)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "DELETE FROM data.presets"
      " WHERE name=?1 AND operation=?2 AND op_version=?3 AND writeprotect=0", -1, &stmt,
      NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, preset, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, module_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, module_version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

gboolean dt_lib_presets_apply(gchar *preset, gchar *module_name, int module_version)
{
  gboolean ret = TRUE;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT op_params, writeprotect"
      " FROM data.presets"
      " WHERE operation = ?1 AND op_version = ?2 AND name = ?3",
      -1, &stmt, NULL);
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
      GList *it = darktable.lib->plugins;
      while(it)
      {
        dt_lib_module_t *module = (dt_lib_module_t *)it->data;
        if(!strncmp(module->plugin_name, module_name, 128))
        {
          gchar *tx = dt_util_dstrcat(NULL, "plugins/darkroom/%s/last_preset", module_name);
          dt_conf_set_string(tx, preset);
          g_free(tx);
          res = module->set_params(module, blob, length);
          break;
        }
        it = g_list_next(it);
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
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM data.presets"
                                " WHERE operation = ?1 AND op_version = ?2 AND name = ?3",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module_version);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, preset, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  return ret;
}

void dt_lib_presets_update(gchar *preset, gchar *module_name, int module_version, const gchar *newname,
                           const gchar *desc, const void *params, const int32_t params_size)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.presets"
                              " SET name = ?1, description = ?2, op_params = ?3"
                              " WHERE operation = ?4 AND op_version = ?5 AND name = ?6",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, newname, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, desc, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, module_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, module_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, preset, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static void pick_callback(GtkMenuItem *menuitem, dt_lib_module_info_t *minfo)
{
  // apply preset via set_params
  char *pn = g_object_get_data(G_OBJECT(menuitem), "dt-preset-name");
  dt_lib_presets_apply(pn, minfo->plugin_name, minfo->version);
}

static void free_module_info(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_info_t *minfo = (dt_lib_module_info_t *)user_data;
  g_free(minfo->plugin_name);
  free(minfo->params);
  free(minfo);
}

static void dt_lib_presets_popup_menu_show(dt_lib_module_info_t *minfo)
{
  GtkMenu *menu = darktable.gui->presets_popup_menu;
  if(menu) gtk_widget_destroy(GTK_WIDGET(menu));
  darktable.gui->presets_popup_menu = GTK_MENU(gtk_menu_new());
  menu = darktable.gui->presets_popup_menu;

  g_signal_connect(G_OBJECT(menu), "destroy", G_CALLBACK(free_module_info), minfo);

  GtkWidget *mi;
  int active_preset = -1, cnt = 0, writeprotect = 0;
  sqlite3_stmt *stmt;
  // order: get shipped defaults first
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name, op_params, writeprotect, description"
                              " FROM data.presets"
                              " WHERE operation=?1 AND op_version=?2"
                              " ORDER BY writeprotect DESC, name, rowid",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, minfo->plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, minfo->version);

  // collect all presets for op from db
  int found = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    void *op_params = (void *)sqlite3_column_blob(stmt, 1);
    int32_t op_params_size = sqlite3_column_bytes(stmt, 1);
    const char *name = (char *)sqlite3_column_text(stmt, 0);

    if(darktable.gui->last_preset && strcmp(darktable.gui->last_preset, name) == 0) found = 1;

    // selected in bold:
    // printf("comparing %d bytes to %d\n", op_params_size, minfo->params_size);
    // for(int k=0;k<op_params_size && !memcmp(minfo->params, op_params, k);k++) printf("compare [%c %c] %d:
    // %d\n",
    // ((const char*)(minfo->params))[k],
    // ((const char*)(op_params))[k],
    // k, memcmp(minfo->params, op_params, k));
    if(op_params_size == minfo->params_size && !memcmp(minfo->params, op_params, op_params_size))
    {
      active_preset = cnt;
      writeprotect = sqlite3_column_int(stmt, 2);
      char *markup;
      mi = gtk_menu_item_new_with_label("");
      markup = g_markup_printf_escaped("<span weight=\"bold\">%s</span>", name);
      gtk_label_set_markup(GTK_LABEL(gtk_bin_get_child(GTK_BIN(mi))), markup);
      g_free(markup);
    }
    else
    {
      mi = gtk_menu_item_new_with_label((const char *)name);
    }
    g_object_set_data_full(G_OBJECT(mi), "dt-preset-name", g_strdup(name), g_free);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(pick_callback), minfo);
    gtk_widget_set_tooltip_text(mi, (const char *)sqlite3_column_text(stmt, 3));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    cnt++;
  }
  sqlite3_finalize(stmt);

  if(cnt > 0) gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

  if(minfo->module->manage_presets)
  {
    mi = gtk_menu_item_new_with_label(_("manage presets..."));
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_manage_presets), minfo);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
  }
  else if(active_preset >= 0) // FIXME: this doesn't seem to work.
  {
    if(!writeprotect)
    {
      mi = gtk_menu_item_new_with_label(_("edit this preset.."));
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_edit_preset), minfo);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

      mi = gtk_menu_item_new_with_label(_("delete this preset"));
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_delete_preset), minfo);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
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
      char *markup = g_markup_printf_escaped("%s <span weight=\"bold\">%s</span>", _("update preset"),
                                             darktable.gui->last_preset);
      mi = gtk_menu_item_new_with_label("");
      gtk_widget_set_sensitive(mi, minfo->params_size > 0);
      gtk_label_set_markup(GTK_LABEL(gtk_bin_get_child(GTK_BIN(mi))), markup);
      g_object_set_data_full(G_OBJECT(mi), "dt-preset-name", g_strdup(darktable.gui->last_preset), g_free);
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_update_preset), minfo);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
      g_free(markup);
    }
  }
}

gint dt_lib_sort_plugins(gconstpointer a, gconstpointer b)
{
  const dt_lib_module_t *am = (const dt_lib_module_t *)a;
  const dt_lib_module_t *bm = (const dt_lib_module_t *)b;
  const int apos = am->position ? am->position(am) : 0;
  const int bpos = bm->position ? bm->position(bm) : 0;
  return apos - bpos;
}

/* default expandable implementation */
static int _lib_default_expandable(dt_lib_module_t *self)
{
  return 1;
}

static int dt_lib_load_module(void *m, const char *libname, const char *plugin_name)
{
  dt_lib_module_t *module = (dt_lib_module_t *)m;
  module->widget = NULL;
  module->expander = NULL;
  g_strlcpy(module->plugin_name, plugin_name, sizeof(module->plugin_name));
  dt_print(DT_DEBUG_CONTROL, "[lib_load_module] loading lib `%s' from %s\n", plugin_name, libname);
  module->module = g_module_open(libname, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
  if(!module->module) goto error;
  int (*version)();
  if(!g_module_symbol(module->module, "dt_module_dt_version", (gpointer) & (version))) goto error;
  if(version() != dt_version())
  {
    fprintf(stderr,
            "[lib_load_module] `%s' is compiled for another version of dt (module %d (%s) != dt %d (%s)) !\n",
            libname, abs(version()), version() < 0 ? "debug" : "opt", abs(dt_version()),
            dt_version() < 0 ? "debug" : "opt");
    goto error;
  }
  if(!g_module_symbol(module->module, "dt_module_mod_version", (gpointer) & (module->version))) goto error;
  if(!g_module_symbol(module->module, "name", (gpointer) & (module->name))) goto error;
  if(!g_module_symbol(module->module, "views", (gpointer) & (module->views))) goto error;
  if(!g_module_symbol(module->module, "container", (gpointer) & (module->container))) goto error;
  if(!g_module_symbol(module->module, "expandable", (gpointer) & (module->expandable)))
    module->expandable = _lib_default_expandable;
  if(!g_module_symbol(module->module, "init", (gpointer) & (module->init))) module->init = NULL;

  if(!g_module_symbol(module->module, "gui_reset", (gpointer) & (module->gui_reset)))
    module->gui_reset = NULL;
  if(!g_module_symbol(module->module, "gui_init", (gpointer) & (module->gui_init))) goto error;
  if(!g_module_symbol(module->module, "gui_cleanup", (gpointer) & (module->gui_cleanup))) goto error;

  if(!g_module_symbol(module->module, "gui_post_expose", (gpointer) & (module->gui_post_expose)))
    module->gui_post_expose = NULL;

  if(!g_module_symbol(module->module, "view_enter", (gpointer) & (module->view_enter))) module->view_enter = NULL;
  if(!g_module_symbol(module->module, "view_leave", (gpointer) & (module->view_leave))) module->view_leave = NULL;

  if(!g_module_symbol(module->module, "mouse_leave", (gpointer) & (module->mouse_leave)))
    module->mouse_leave = NULL;
  if(!g_module_symbol(module->module, "mouse_moved", (gpointer) & (module->mouse_moved)))
    module->mouse_moved = NULL;
  if(!g_module_symbol(module->module, "button_released", (gpointer) & (module->button_released)))
    module->button_released = NULL;
  if(!g_module_symbol(module->module, "button_pressed", (gpointer) & (module->button_pressed)))
    module->button_pressed = NULL;
  if(!g_module_symbol(module->module, "configure", (gpointer) & (module->configure)))
    module->configure = NULL;
  if(!g_module_symbol(module->module, "scrolled", (gpointer) & (module->scrolled))) module->scrolled = NULL;
  if(!g_module_symbol(module->module, "position", (gpointer) & (module->position))) module->position = NULL;
  if(!g_module_symbol(module->module, "legacy_params", (gpointer) & (module->legacy_params)))
    module->legacy_params = NULL;
  if((!g_module_symbol(module->module, "get_params", (gpointer) & (module->get_params)))
     || (!g_module_symbol(module->module, "set_params", (gpointer) & (module->set_params)))
     || (!g_module_symbol(module->module, "init_presets", (gpointer) & (module->init_presets))))
  {
    // need all at the same time, or none.
    module->legacy_params = NULL;
    module->set_params = NULL;
    module->get_params = NULL;
    module->init_presets = NULL;
  }
  if(!module->init_presets
     || !g_module_symbol(module->module, "manage_presets", (gpointer) & (module->manage_presets)))
    module->manage_presets = NULL;
  if(!g_module_symbol(module->module, "init_key_accels", (gpointer) & (module->init_key_accels)))
    module->init_key_accels = NULL;
  if(!g_module_symbol(module->module, "connect_key_accels", (gpointer) & (module->connect_key_accels)))
    module->connect_key_accels = NULL;

  module->accel_closures = NULL;
  module->reset_button = NULL;
  module->presets_button = NULL;

  if(module->gui_reset)
  {
    dt_accel_register_lib(module, NC_("accel", "reset module parameters"), 0, 0);
  }
  if(module->get_params)
  {
    dt_accel_register_lib(module, NC_("accel", "show preset menu"), 0, 0);
  }
  if(module->expandable(module))
  {
    dt_accel_register_lib(module, NC_("accel", "show module"), 0, 0);
  }
#ifdef USE_LUA
  dt_lua_lib_register(darktable.lua_state.state, module);
#endif
  if(module->init) module->init(module);

  return 0;
error:
  fprintf(stderr, "[lib_load_module] failed to open operation `%s': %s\n", plugin_name, g_module_error());
  if(module->module) g_module_close(module->module);
  return 1;
}

static void *_update_params(dt_lib_module_t *module,
                            const void *const old_params, size_t old_params_size, int old_version,
                            int target_version, size_t *new_size)
{
  // make a copy of the old params so we can free it in the loop
  void *params = malloc(old_params_size);
  if(params == NULL) return NULL;
  memcpy(params, old_params, old_params_size);
  while(old_version < target_version)
  {
    size_t size;
    int version;
    void *new_params = module->legacy_params(module, params, old_params_size, old_version, &version, &size);
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
  // since lighttable presets can't end up in styles or any other place outside of the presets table it is
  // sufficient
  // to update that very table here and assume that everything is up to date elsewhere.
  // the intended logic is as follows:
  // - no set_params -> delete all presets
  // - op_version >= module_version -> done
  // - op_version < module_version ->
  //   - module has legacy_params -> try to update
  //   - module doesn't have legacy_params -> delete it

  if(module->set_params == NULL)
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM data.presets"
                                " WHERE operation=?1", -1,
                                &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->plugin_name, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  else
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT rowid, op_version, op_params, name"
                                " FROM data.presets"
                                " WHERE operation=?1",
                                -1, &stmt, NULL);
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
          && (new_params = _update_params(module, op_params, op_params_size, op_version, version, &new_params_size)))
        {
          // write the updated preset back to db
          fprintf(stderr,
                  "[lighttable_init_presets] updating '%s' preset '%s' from version %d to version %d\n",
                  module->plugin_name, name, op_version, version);
          sqlite3_stmt *innerstmt;
          DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                      "UPDATE data.presets"
                                      " SET op_version=?1, op_params=?2"
                                      " WHERE rowid=?3", -1,
                                      &innerstmt, NULL);
          DT_DEBUG_SQLITE3_BIND_INT(innerstmt, 1, version);
          DT_DEBUG_SQLITE3_BIND_BLOB(innerstmt, 2, new_params, new_params_size, SQLITE_TRANSIENT);
          DT_DEBUG_SQLITE3_BIND_INT(innerstmt, 3, rowid);
          sqlite3_step(innerstmt);
          sqlite3_finalize(innerstmt);
        }
        else
        {
          // delete the preset
          fprintf(stderr, "[lighttable_init_presets] Can't upgrade '%s' preset '%s' from version %d to %d, "
                          "no legacy_params() implemented or unable to update\n",
                  module->plugin_name, name, op_version, version);
          sqlite3_stmt *innerstmt;
          DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                      "DELETE FROM data.presets"
                                      " WHERE rowid=?1", -1,
                                      &innerstmt, NULL);
          DT_DEBUG_SQLITE3_BIND_INT(innerstmt, 1, rowid);
          sqlite3_step(innerstmt);
          sqlite3_finalize(innerstmt);
        }
        free(new_params);
      }
    }
    sqlite3_finalize(stmt);
  }

  if(module->init_presets) module->init_presets(module);
}

static void dt_lib_init_module(void *m)
{
  dt_lib_module_t *module = (dt_lib_module_t *)m;
  dt_lib_init_presets(module);
  // Calling the keyboard shortcut initialization callback if present
  // do not init accelerators if there is no gui
  if(darktable.gui)
  {
    if(module->init_key_accels) module->init_key_accels(module);
    module->gui_init(module);
    g_object_ref_sink(module->widget);
  }
}

void dt_lib_unload_module(dt_lib_module_t *module)
{
  if(module->module) g_module_close(module->module);
}

static void dt_lib_gui_reset_callback(GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *module = (dt_lib_module_t *)user_data;
  module->gui_reset(module);
}

#if !GTK_CHECK_VERSION(3, 22, 0)
static void _preset_popup_posistion(GtkMenu *menu, gint *x, gint *y, gboolean *push_in, gpointer data)
{
  gint w;
  gint ww;
  GtkRequisition requisition = { 0 };

  w = gdk_window_get_width(gtk_widget_get_window(GTK_WIDGET(data)));
  ww = gdk_window_get_width(gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui)));

  gdk_window_get_origin(gtk_widget_get_window(GTK_WIDGET(data)), x, y);

  gtk_widget_get_preferred_size(GTK_WIDGET(menu), &requisition, NULL);

  /* align left panel popupmenu to right edge */
  if(*x < ww / 2) (*x) += w - requisition.width;

  GtkAllocation allocation_data;
  gtk_widget_get_allocation(GTK_WIDGET(data), &allocation_data);
  (*y) += allocation_data.height;
}
#endif

static void popup_callback(GtkButton *button, dt_lib_module_t *module)
{
  dt_lib_module_info_t *mi = (dt_lib_module_info_t *)calloc(1, sizeof(dt_lib_module_info_t));

  mi->plugin_name = g_strdup(module->plugin_name);
  mi->version = module->version();
  mi->module = module;
  mi->params = module->get_params(module, &mi->params_size);

  if(!mi->params)
  {
    // this is a valid case, for example in location.c when nothing got selected
    // fprintf(stderr, "something went wrong: &params=%p, size=%i\n", mi->params, mi->params_size);
    mi->params_size = 0;
  }
  dt_lib_presets_popup_menu_show(mi);

  gtk_widget_show_all(GTK_WIDGET(darktable.gui->presets_popup_menu));

#if GTK_CHECK_VERSION(3, 22, 0)
  GdkGravity widget_gravity, menu_gravity;
  widget_gravity = GDK_GRAVITY_SOUTH_EAST;
  menu_gravity = GDK_GRAVITY_NORTH_EAST;
  GtkWidget *w = module->presets_button;
  if(module->expander) w = dtgtk_expander_get_header(DTGTK_EXPANDER(module->expander));

  gtk_menu_popup_at_widget(darktable.gui->presets_popup_menu, w, widget_gravity, menu_gravity, NULL);
#else
  gtk_menu_popup(darktable.gui->presets_popup_menu, NULL, NULL, _preset_popup_posistion, button, 0,
                 gtk_get_current_event_time());
  gtk_menu_reposition(GTK_MENU(darktable.gui->presets_popup_menu));
#endif

  dtgtk_button_set_active(DTGTK_BUTTON(button), FALSE);
}


void dt_lib_gui_set_expanded(dt_lib_module_t *module, gboolean expanded)
{
  if(!module->expander) return;

  dtgtk_expander_set_expanded(DTGTK_EXPANDER(module->expander), expanded);

  /* update expander arrow state */
  GtkDarktableButton *icon;
  GtkWidget *header = dtgtk_expander_get_header(DTGTK_EXPANDER(module->expander));
  gint flags = CPF_DIRECTION_DOWN | CPF_BG_TRANSPARENT | CPF_STYLE_FLAT;

  GList *header_childs = gtk_container_get_children(GTK_CONTAINER(header));
  icon = g_list_nth_data(header_childs, DT_MODULE_ARROW);
  if(!expanded) flags = CPF_DIRECTION_RIGHT | CPF_BG_TRANSPARENT | CPF_STYLE_FLAT;
  g_list_free(header_childs);
  dtgtk_button_set_paint(icon, dtgtk_cairo_paint_solid_arrow, flags, NULL);

  /* show / hide plugin widget */
  if(expanded)
  {
    /* register to receive draw events */
    darktable.lib->gui_module = module;

    if(dt_conf_get_bool("darkroom/ui/scroll_to_module"))
      darktable.gui->scroll_to[1] = module->expander;
  }
  else
  {
    if(darktable.lib->gui_module == module)
    {
      darktable.lib->gui_module = NULL;

      dt_control_queue_redraw();
    }
  }

  /* store expanded state of module */
  char var[1024];
  const dt_view_t *current_view = dt_view_manager_get_current_view(darktable.view_manager);
  snprintf(var, sizeof(var), "plugins/%s/%s/expanded", current_view->module_name, module->plugin_name);
  dt_conf_set_bool(var, expanded);
}

gboolean dt_lib_gui_get_expanded(dt_lib_module_t *module)
{
  if(!module->expandable(module)) return true;
  if(!module->expander) return true;
  if(!module->widget)
  {
    char var[1024];
    const dt_view_t *current_view = dt_view_manager_get_current_view(darktable.view_manager);
    snprintf(var, sizeof(var), "plugins/%s/%s/expanded", current_view->module_name, module->plugin_name);
    return dt_conf_get_bool(var);
  }
  return dtgtk_expander_get_expanded(DTGTK_EXPANDER(module->expander));
}

static gboolean _lib_plugin_header_button_press(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  if(e->type == GDK_2BUTTON_PRESS || e->type == GDK_3BUTTON_PRESS) return TRUE;

  dt_lib_module_t *module = (dt_lib_module_t *)user_data;

  if(e->button == 1)
  {
    /* bail out if module is static */
    if(!module->expandable(module)) return FALSE;

    // make gtk scroll to the module once it updated its allocation size
    uint32_t container = module->container(module);
    if(dt_conf_get_bool("lighttable/ui/scroll_to_module"))
    {
      if(container == DT_UI_CONTAINER_PANEL_LEFT_CENTER)
        darktable.gui->scroll_to[0] = module->expander;
      else if(container == DT_UI_CONTAINER_PANEL_RIGHT_CENTER)
        darktable.gui->scroll_to[1] = module->expander;
    }

    /* handle shiftclick on expander, hide all except this */
    if(!dt_conf_get_bool("lighttable/ui/single_module") != !(e->state & GDK_SHIFT_MASK))
    {
      GList *it = g_list_first(darktable.lib->plugins);
      const dt_view_t *v = dt_view_manager_get_current_view(darktable.view_manager);
      gboolean all_other_closed = TRUE;
      while(it)
      {
        dt_lib_module_t *m = (dt_lib_module_t *)it->data;

        if(m != module && container == m->container(m) && m->expandable(m) && dt_lib_is_visible_in_view(m, v))
        {
          all_other_closed = all_other_closed && !dtgtk_expander_get_expanded(DTGTK_EXPANDER(m->expander));
          dt_lib_gui_set_expanded(m, FALSE);
        }

        it = g_list_next(it);
      }
      if(all_other_closed)
        dt_lib_gui_set_expanded(module, !dtgtk_expander_get_expanded(DTGTK_EXPANDER(module->expander)));
      else
        dt_lib_gui_set_expanded(module, TRUE);
    }
    else
    {
      /* else just toggle */
      dt_lib_gui_set_expanded(module, !dtgtk_expander_get_expanded(DTGTK_EXPANDER(module->expander)));
    }

    //ensure that any gtkentry fields lose focus
    gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

    return TRUE;
  }
  else if(e->button == 2)
  {
    /* show preset popup if any preset for module */

    return TRUE;
  }
  return FALSE;
}

static gboolean show_module_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                     GdkModifierType modifier, gpointer data)

{
  dt_lib_module_t *module = (dt_lib_module_t *)data;

  /* bail out if module is static */
  if(!module->expandable(module)) return FALSE;

  // make gtk scroll to the module once it updated its allocation size
  uint32_t container = module->container(module);
  if(dt_conf_get_bool("lighttable/ui/scroll_to_module"))
  {
    if(container == DT_UI_CONTAINER_PANEL_LEFT_CENTER)
      darktable.gui->scroll_to[0] = module->expander;
    else if(container == DT_UI_CONTAINER_PANEL_RIGHT_CENTER)
      darktable.gui->scroll_to[1] = module->expander;
  }

  if(dt_conf_get_bool("lighttable/ui/single_module"))
  {
    GList *it = g_list_first(darktable.lib->plugins);
    const dt_view_t *v = dt_view_manager_get_current_view(darktable.view_manager);
    gboolean all_other_closed = TRUE;
    while(it)
    {
      dt_lib_module_t *m = (dt_lib_module_t *)it->data;

      if(m != module && container == m->container(m) && m->expandable(m) && dt_lib_is_visible_in_view(m, v))
      {
        all_other_closed = all_other_closed && !dtgtk_expander_get_expanded(DTGTK_EXPANDER(m->expander));
        dt_lib_gui_set_expanded(m, FALSE);
      }

      it = g_list_next(it);
    }
    if(all_other_closed)
      dt_lib_gui_set_expanded(module, !dtgtk_expander_get_expanded(DTGTK_EXPANDER(module->expander)));
    else
      dt_lib_gui_set_expanded(module, TRUE);
    }
    else
    {
      /* else just toggle */
      dt_lib_gui_set_expanded(module, !dtgtk_expander_get_expanded(DTGTK_EXPANDER(module->expander)));
    }
  return TRUE;
}

GtkWidget *dt_lib_gui_get_expander(dt_lib_module_t *module)
{
  /* check if module is expandable */
  if(!module->expandable(module))
  {
    if(module->presets_button)
    {
      // if presets btn has been loaded to be shown outside expander
      g_signal_connect(G_OBJECT(module->presets_button), "clicked", G_CALLBACK(popup_callback), module);
    }
    module->expander = NULL;
    return NULL;
  }

  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(GTK_WIDGET(header), "module-header");

  GtkWidget *expander = dtgtk_expander_new(header, module->widget);
  GtkWidget *header_evb = dtgtk_expander_get_header_event_box(DTGTK_EXPANDER(expander));
  GtkWidget *pluginui_frame = dtgtk_expander_get_frame(DTGTK_EXPANDER(expander));

  /* setup the header box */
  g_signal_connect(G_OBJECT(header_evb), "button-press-event", G_CALLBACK(_lib_plugin_header_button_press),
                   module);

  /*
   * initialize the header widgets
   */
  GtkWidget *hw[DT_MODULE_LAST] = { NULL };

  /* add the expand indicator icon */
  hw[DT_MODULE_ARROW] = dtgtk_button_new(dtgtk_cairo_paint_solid_arrow, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_name(GTK_WIDGET(hw[DT_MODULE_ARROW]), "module-collapse-button");
  g_signal_connect(G_OBJECT(hw[DT_MODULE_ARROW]), "button-press-event", G_CALLBACK(_lib_plugin_header_button_press),
                   module);


  /* add module label */
  hw[DT_MODULE_LABEL] = gtk_label_new("");
  gtk_label_set_markup(GTK_LABEL(hw[DT_MODULE_LABEL]), module->name(module));
  gtk_widget_set_tooltip_text(hw[DT_MODULE_LABEL], module->name(module));
  gtk_label_set_ellipsize(GTK_LABEL(hw[DT_MODULE_LABEL]), PANGO_ELLIPSIZE_END);
  g_object_set(G_OBJECT(hw[DT_MODULE_LABEL]), "xalign", 0.0, (gchar *)0);
  gtk_widget_set_name(hw[DT_MODULE_LABEL], "lib-panel-label");

  /* add reset button if module has implementation */
  hw[DT_MODULE_RESET] = dtgtk_button_new(dtgtk_cairo_paint_reset, CPF_STYLE_FLAT, NULL);
  module->reset_button = GTK_WIDGET(hw[DT_MODULE_RESET]);
  gtk_widget_set_tooltip_text(hw[DT_MODULE_RESET], _("reset parameters"));
  g_signal_connect(G_OBJECT(hw[DT_MODULE_RESET]), "clicked", G_CALLBACK(dt_lib_gui_reset_callback), module);

  if(!module->gui_reset) gtk_widget_set_sensitive(GTK_WIDGET(hw[DT_MODULE_RESET]), FALSE);
  gtk_widget_set_name(GTK_WIDGET(hw[DT_MODULE_RESET]), "module-reset-button");

  /* add preset button if module has implementation */
  hw[DT_MODULE_PRESETS] = dtgtk_button_new(dtgtk_cairo_paint_presets, CPF_STYLE_FLAT, NULL);
  module->presets_button = GTK_WIDGET(hw[DT_MODULE_PRESETS]);
  gtk_widget_set_tooltip_text(hw[DT_MODULE_PRESETS], _("presets"));
  g_signal_connect(G_OBJECT(hw[DT_MODULE_PRESETS]), "clicked", G_CALLBACK(popup_callback), module);

  if(!module->get_params) gtk_widget_set_sensitive(GTK_WIDGET(hw[DT_MODULE_PRESETS]), FALSE);
  gtk_widget_set_name(GTK_WIDGET(hw[DT_MODULE_PRESETS]), "module-preset-button");

  /* lets order header elements depending on left/right side panel placement */

  for(int i = 0; i < DT_MODULE_LAST; i++)
    if(hw[i]) gtk_box_pack_start(GTK_BOX(header), hw[i], i == DT_MODULE_LABEL ? TRUE : FALSE, i == DT_MODULE_LABEL ? TRUE : FALSE, 0);
  gtk_widget_set_halign(hw[DT_MODULE_ARROW], GTK_ALIGN_START);
  gtk_widget_set_halign(hw[DT_MODULE_LABEL], GTK_ALIGN_START);
  gtk_widget_set_halign(hw[DT_MODULE_RESET], GTK_ALIGN_END);

  gtk_widget_show_all(module->widget);
  gtk_widget_set_name(module->widget, "lib-plugin-ui-main");
  gtk_widget_set_name(pluginui_frame, "lib-plugin-ui");
  module->expander = expander;

  gtk_widget_set_hexpand(module->widget, FALSE);
  gtk_widget_set_vexpand(module->widget, FALSE);

  return module->expander;
}

void dt_lib_init(dt_lib_t *lib)
{
  // Setting everything to null initially
  memset(lib, 0, sizeof(dt_lib_t));
  darktable.lib->plugins = dt_module_load_modules("/plugins/lighttable", sizeof(dt_lib_module_t),
                                                  dt_lib_load_module, dt_lib_init_module, dt_lib_sort_plugins);
}

void dt_lib_cleanup(dt_lib_t *lib)
{
  while(lib->plugins)
  {
    dt_lib_module_t *module = (dt_lib_module_t *)(lib->plugins->data);
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

void dt_lib_presets_add(const char *name, const char *plugin_name, const int32_t version, const void *params,
                        const int32_t params_size, gboolean readonly)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM data.presets"
                              " WHERE name=?1 AND operation=?2 AND op_version=?3",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "INSERT INTO data.presets"
      " (name, description, operation, op_version, op_params, "
      "  blendop_params, blendop_version, enabled, model, maker, lens, "
      "  iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max, "
      "  focal_length_min, focal_length_max, writeprotect, "
      "  autoapply, filter, def, format)"
      " VALUES "
      "  (?1, '', ?2, ?3, ?4, NULL, 0, 1, '%', "
      "   '%', '%', 0, 340282346638528859812000000000000000000, 0, 10000000, 0, 100000000, 0,"
      "   1000, ?5, 0, 0, 0, 0)",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, readonly);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static gchar *_get_lib_view_path(dt_lib_module_t *module, char *suffix)
{
  if(!darktable.view_manager) return NULL;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  // in lighttable, we store panels states per layout
  char lay[32] = "";
  if(g_strcmp0(cv->module_name, "lighttable") == 0)
  {
    if(dt_view_lighttable_preview_state(darktable.view_manager))
      g_snprintf(lay, sizeof(lay), "preview/");
    else
      g_snprintf(lay, sizeof(lay), "%d/", dt_view_lighttable_get_layout(darktable.view_manager));
  }
  else if(g_strcmp0(cv->module_name, "darkroom") == 0)
  {
    g_snprintf(lay, sizeof(lay), "%d/", dt_view_darkroom_get_layout(darktable.view_manager));
  }

  return dt_util_dstrcat(NULL, "plugins/%s/%s%s%s", cv->module_name, lay, module->plugin_name, suffix);
}

gboolean dt_lib_is_visible(dt_lib_module_t *module)
{
  gchar *key = _get_lib_view_path(module, "_visible");
  gboolean ret = TRUE; /* if not key found, always make module visible */
  if(key && dt_conf_key_exists(key)) ret = dt_conf_get_bool(key);
  g_free(key);

  return ret;
}

void dt_lib_set_visible(dt_lib_module_t *module, gboolean visible)
{
  gchar *key = _get_lib_view_path(module, "_visible");
  dt_conf_set_bool(key, visible);
  g_free(key);
  if(module->widget)
  {
    if(module->expander)
    {
      dtgtk_expander_set_expanded(DTGTK_EXPANDER(module->expander), visible);
    }
    else
    {
      if(visible)
        gtk_widget_show_all(GTK_WIDGET(module->widget));
      else
        gtk_widget_hide(GTK_WIDGET(module->widget));
    }
  }
}

void dt_lib_connect_common_accels(dt_lib_module_t *module)
{
  if(module->reset_button)
    dt_accel_connect_button_lib(module, "reset module parameters", module->reset_button);
  if(module->presets_button) dt_accel_connect_button_lib(module, "show preset menu", module->presets_button);
  if(module->expandable(module))
  {
    GClosure *closure = NULL;
    closure = g_cclosure_new(G_CALLBACK(show_module_callback), module, NULL);
    dt_accel_connect_lib(module, "show module", closure);
  }
  if(module->init_presets)
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT name"
                                " FROM data.presets"
                                " WHERE operation=?1 AND op_version=?2"
                                " ORDER BY writeprotect DESC, name, rowid",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->plugin_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      char path[1024];
      snprintf(path, sizeof(path), "%s/%s", _("preset"), (char *)sqlite3_column_text(stmt, 0));
      dt_accel_register_lib(module, path, 0, 0);
      dt_accel_connect_preset_lib(module, (char *)sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
  }
}

gchar *dt_lib_get_localized_name(const gchar *plugin_name)
{
  // Prepare mapping op -> localized name
  static GHashTable *module_names = NULL;
  if(module_names == NULL)
  {
    module_names = g_hash_table_new(g_str_hash, g_str_equal);
    GList *lib = g_list_first(darktable.lib->plugins);
    if(lib != NULL)
    {
      do
      {
        dt_lib_module_t *module = (dt_lib_module_t *)lib->data;
        g_hash_table_insert(module_names, module->plugin_name, g_strdup(module->name(module)));
      } while((lib = g_list_next(lib)) != NULL);
    }
  }

  return (gchar *)g_hash_table_lookup(module_names, plugin_name);
}

void dt_lib_colorpicker_set_area(dt_lib_t *lib, float size)
{
  if(!lib->proxy.colorpicker.module || !lib->proxy.colorpicker.set_sample_area) return;
  lib->proxy.colorpicker.set_sample_area(lib->proxy.colorpicker.module, size);
  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
}

void dt_lib_colorpicker_set_box_area(dt_lib_t *lib, const float *const box)
{
  if(!lib->proxy.colorpicker.module || !lib->proxy.colorpicker.set_sample_box_area) return;
  lib->proxy.colorpicker.set_sample_box_area(lib->proxy.colorpicker.module, box);
  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
}

void dt_lib_colorpicker_set_point(dt_lib_t *lib, float x, float y)
{
  if(!lib->proxy.colorpicker.module || !lib->proxy.colorpicker.set_sample_point) return;
  lib->proxy.colorpicker.set_sample_point(lib->proxy.colorpicker.module, x, y);
  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
}

dt_lib_module_t *dt_lib_get_module(const char *name)
{
  /* hide/show modules as last config */
  for(GList *iter = darktable.lib->plugins; iter; iter = g_list_next(iter))
  {
    dt_lib_module_t *plugin = (dt_lib_module_t *)(iter->data);
    if(strcmp(plugin->plugin_name, name) == 0)
      return plugin;
  }

  return NULL;
}

/* callback function for delayed update after user interaction */
static gboolean _postponed_update(gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  self->timeout_handle = 0;
  if (self->_postponed_update)
    self->_postponed_update(self);

  return FALSE; // cancel the timer
}

/** queue a delayed call of update function after user interaction */
void dt_lib_queue_postponed_update(dt_lib_module_t *mod, void (*update_fn)(dt_lib_module_t *self))
{
  if(mod->timeout_handle)
  {
    // here we're making sure the event fires at last hover
    // and we won't have avalanche of events in the mean time.
    g_source_remove(mod->timeout_handle);
  }
  const int delay = CLAMP(darktable.develop->average_delay / 2, 10, 250);
  mod->_postponed_update = update_fn;
  mod->timeout_handle = g_timeout_add(delay, _postponed_update, mod);
}

void dt_lib_cancel_postponed_update(dt_lib_module_t *mod)
{
  mod->_postponed_update = NULL;
  if (mod->timeout_handle)
  {
    g_source_remove(mod->timeout_handle);
    mod->timeout_handle = 0;
  }
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
