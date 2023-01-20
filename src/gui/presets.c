/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/presets.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/guides.h"
#include "gui/presets.h"
#include "libs/modulegroups.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <assert.h>
#include <stdlib.h>


const int dt_gui_presets_exposure_value_cnt = 24;
const float dt_gui_presets_exposure_value[]
    = { 0.,       1. / 8000, 1. / 4000, 1. / 2000, 1. / 1000, 1. / 1000, 1. / 500, 1. / 250,
        1. / 125, 1. / 60,   1. / 30,   1. / 15,   1. / 15,   1. / 8,    1. / 4,   1. / 2,
        1,        2,         4,         8,         15,        30,        60,       FLT_MAX };
const char *dt_gui_presets_exposure_value_str[]
    = { "0",     "1/8000", "1/4000", "1/2000", "1/1000", "1/1000", "1/500", "1/250",
        "1/125", "1/60",   "1/30",   "1/15",   "1/15",   "1/8",    "1/4",   "1/2",
        "1\"",   "2\"",    "4\"",    "8\"",    "15\"",   "30\"",   "60\"",  "+" };
const int dt_gui_presets_aperture_value_cnt = 19;
const float dt_gui_presets_aperture_value[]
    = { 0, 0.5, 0.7, 1.0, 1.4, 2.0, 2.8, 4.0, 5.6, 8.0, 11.0, 16.0, 22.0, 32.0, 45.0, 64.0, 90.0, 128.0, FLT_MAX };
const char *dt_gui_presets_aperture_value_str[]
    = { "f/0",  "f/0.5", "f/0.7", "f/1.0", "f/1.4", "f/2",  "f/2.8", "f/4",   "f/5.6", "f/8",
        "f/11", "f/16",  "f/22",  "f/32",  "f/45",  "f/64", "f/90",  "f/128", "f/+" };

// format string and corresponding flag stored into the database
static const char *_gui_presets_format_value_str[5]
    = { N_("non-raw"), N_("raw"), N_("HDR"), N_("monochrome"), N_("color") };
static const int _gui_presets_format_flag[5] = { FOR_LDR, FOR_RAW, FOR_HDR, FOR_NOT_MONO, FOR_NOT_COLOR };

// this is also called for non-gui applications linking to libdarktable!
// so beware, don't use any darktable.gui stuff here .. (or change this behaviour in darktable.c)
void dt_gui_presets_init()
{
  // remove auto generated presets from plugins, not the user included ones.
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM data.presets WHERE writeprotect = 1", NULL,
                        NULL, NULL);
}

void dt_gui_presets_add_generic(const char *name, dt_dev_operation_t op, const int32_t version,
                                const void *params, const int32_t params_size,
                                const int32_t enabled,
                                const dt_develop_blend_colorspace_t blend_cst)
{
  dt_develop_blend_params_t default_blendop_params;
  dt_develop_blend_init_blend_parameters(&default_blendop_params, blend_cst);
  dt_gui_presets_add_with_blendop(
      name, op, version, params, params_size,
      &default_blendop_params, enabled);
}

void dt_gui_presets_add_with_blendop(
    const char *name, dt_dev_operation_t op, const int32_t version,
    const void *params, const int32_t params_size,
    const void *blend_params, const int32_t enabled)
{
  sqlite3_stmt *stmt;

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "INSERT OR REPLACE"
      " INTO data.presets (name, description, operation, op_version, op_params, enabled,"
      "                    blendop_params, blendop_version, multi_priority, multi_name,"
      "                    model, maker, lens, iso_min, iso_max, exposure_min, exposure_max,"
      "                    aperture_min, aperture_max, focal_length_min, focal_length_max,"
      "                    writeprotect, autoapply, filter, def, format)"
      " VALUES (?1, '', ?2, ?3, ?4, ?5, ?6, ?7, 0, '', '%', '%', '%', 0,"
      "         340282346638528859812000000000000000000, 0, 10000000, 0, 100000000, 0,"
      "         1000, 1, 0, 0, 0, 0)",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, enabled);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, blend_params, sizeof(dt_develop_blend_params_t),
                             SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, dt_develop_blend_version());
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static gchar *_get_active_preset_name(dt_iop_module_t *module, int *writeprotect)
{
  sqlite3_stmt *stmt;
  // if we sort by writeprotect DESC then in case user copied the writeprotected preset
  // then the preset name returned will be writeprotected and thus not deletable
  // sorting ASC prefers user created presets.
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
     "SELECT name, op_params, blendop_params, enabled, writeprotect"
     " FROM data.presets"
     " WHERE operation=?1 AND op_version=?2"
     " ORDER BY writeprotect ASC, LOWER(name), rowid",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
  gchar *name = NULL;

  // collect all presets for op from db
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *op_params = (void *)sqlite3_column_blob(stmt, 1);
    const int32_t op_params_size = sqlite3_column_bytes(stmt, 1);
    const void *blendop_params = (void *)sqlite3_column_blob(stmt, 2);
    const int32_t bl_params_size = sqlite3_column_bytes(stmt, 2);
    const int enabled = sqlite3_column_int(stmt, 3);

    if(!memcmp(module->params, op_params, MIN(op_params_size, module->params_size))
       && !memcmp(module->blend_params, blendop_params,
                  MIN(bl_params_size, sizeof(dt_develop_blend_params_t))) && module->enabled == enabled)
    {
      name = g_strdup((char *)sqlite3_column_text(stmt, 0));
      *writeprotect = sqlite3_column_int(stmt, 4);
      break;
    }
  }
  sqlite3_finalize(stmt);
  return name;
}

static void _menuitem_delete_preset(GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  int writeprotect = -1;
  gchar *name = _get_active_preset_name(module, &writeprotect);
  if(name == NULL) return;

  if(writeprotect)
  {
    dt_control_log(_("preset `%s' is write-protected, can't delete!"), name);
    g_free(name);
    return;
  }

  if(!dt_conf_get_bool("plugins/lighttable/preset/ask_before_delete_preset")
     || dt_gui_show_yes_no_dialog(_("delete preset?"),
                                  _("do you really want to delete the preset `%s'?"), name))
  {
    dt_action_rename_preset(&module->so->actions, name, NULL);

    dt_lib_presets_remove(name, module->op, module->version());
  }
  g_free(name);
}
static void _edit_preset_final_callback(dt_gui_presets_edit_dialog_t *g)
{
  dt_gui_store_last_preset(gtk_entry_get_text(g->name));
}

static void _edit_preset_response(GtkDialog *dialog, gint response_id, dt_gui_presets_edit_dialog_t *g)
{
  if(response_id == GTK_RESPONSE_OK)
  {
    // find the module action list this preset belongs to
    dt_action_t *module_actions = g->iop ? &g->iop->so->actions : NULL;

    for(GList *libs = darktable.lib->plugins; !module_actions && libs; libs = g_list_next(libs))
    {
      dt_lib_module_t *lib = libs->data;

      if(!strcmp(lib->plugin_name, g->operation))
        module_actions = &lib->actions;
    }

    // we want to save the preset in the database
    sqlite3_stmt *stmt;

    // we verify eventual name collisions
    const gchar *name = gtk_entry_get_text(g->name);
    if(((g->old_id >= 0)
        && (strcmp(g->original_name, name) != 0))
       || (g->old_id < 0))
    {
      if(name == NULL || *name == '\0' || strcmp(_("new preset"), name) == 0)
      {
        // show error dialog
        GtkWidget *dlg_changename
            = gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                     GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, _("please give preset a name"));
#ifdef GDK_WINDOWING_QUARTZ
        dt_osx_disallow_fullscreen(dlg_changename);
#endif

        gtk_window_set_title(GTK_WINDOW(dlg_changename), _("unnamed preset"));

        gtk_dialog_run(GTK_DIALOG(dlg_changename));
        gtk_widget_destroy(dlg_changename);
        return;
      }

      // editing existing preset with different name or store new
      // preset -> check for a preset with the same name:
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(
          dt_database_get(darktable.db),
          "SELECT name"
          " FROM data.presets"
          " WHERE name = ?1 AND operation=?2 AND op_version=?3"
          " LIMIT 1",
          -1, &stmt, NULL);
      // clang-format on
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, g->operation, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, g->op_version);

      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        sqlite3_finalize(stmt);

        // if result is BUTTON_NO or ESCAPE keypress exit without destroying dialog, to permit other name
        if(dt_gui_show_yes_no_dialog(_("overwrite preset?"),
                                     _("preset `%s' already exists.\ndo you want to overwrite?"), name))
        {
          // we remove the preset that will be overwrite
          dt_lib_presets_remove(name, g->operation, g->op_version);

          dt_action_rename_preset(module_actions, name, NULL);
        }
        else
          return;
      }
      else
      {
        sqlite3_finalize(stmt);
      }
    }

    gchar *query = NULL;
    if(g->old_id >= 0)
    {
      // we update presets values
      // clang-format off
      query = g_strdup_printf
        ("UPDATE data.presets "
         "SET"
         " name=?1, description=?2,"
         " model=?3, maker=?4, lens=?5, iso_min=?6, iso_max=?7, exposure_min=?8,"
         " exposure_max=?9, aperture_min=?10,"
         " aperture_max=?11, focal_length_min=?12, focal_length_max=?13, autoapply=?14,"
         " filter=?15, format=?16, multi_name=?23, multi_name_hand_edited=?24 "
         "WHERE rowid=%d",
         g->old_id);
      // clang-format on
    }
    else
    {
      // we create a new preset
      // clang-format off
      query = g_strdup_printf
        ("INSERT INTO data.presets"
         " (name, description, "
         "  model, maker, lens, iso_min, iso_max, exposure_min, exposure_max, aperture_min,"
         "  aperture_max, focal_length_min, focal_length_max, autoapply,"
         "  filter, format, def, writeprotect, operation, op_version, op_params, enabled,"
         "  blendop_params, blendop_version,"
         "   multi_priority, multi_name, multi_name_hand_edited) "
         "VALUES"
         " (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16,"
         "   0, 0, ?17, ?18, ?19, ?20, ?21, ?22, 0, ?23, ?24)");
      // clang-format on
    }

    // rename accelerators
    dt_action_rename_preset(module_actions, g->original_name, name);

    // commit all the user input fields
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    g_free(query);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, gtk_entry_get_text(g->description), -1, SQLITE_TRANSIENT);

    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, gtk_entry_get_text(GTK_ENTRY(g->model)), -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, gtk_entry_get_text(GTK_ENTRY(g->maker)), -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, gtk_entry_get_text(GTK_ENTRY(g->lens)), -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 6, gtk_spin_button_get_value(GTK_SPIN_BUTTON(g->iso_min)));
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, gtk_spin_button_get_value(GTK_SPIN_BUTTON(g->iso_max)));
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, dt_gui_presets_exposure_value[dt_bauhaus_combobox_get(g->exposure_min)]);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, dt_gui_presets_exposure_value[dt_bauhaus_combobox_get(g->exposure_max)]);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10,
                                 dt_gui_presets_aperture_value[dt_bauhaus_combobox_get(g->aperture_min)]);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 11,
                                 dt_gui_presets_aperture_value[dt_bauhaus_combobox_get(g->aperture_max)]);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 12, gtk_spin_button_get_value(GTK_SPIN_BUTTON(g->focal_length_min)));
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 13, gtk_spin_button_get_value(GTK_SPIN_BUTTON(g->focal_length_max)));
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 14, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->autoapply)));
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 15, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->filter)));
    int format = 0;
    for(int k = 0; k < 5; k++)
      format += gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->format_btn[k])) * _gui_presets_format_flag[k];
    format ^= DT_PRESETS_FOR_NOT;

    DT_DEBUG_SQLITE3_BIND_INT(stmt, 16, format);

    // commit specific field in case of newly crearted preset
    if(g->old_id < 0)
    {
      if(g->iop)
      {
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 17, g->operation, -1, SQLITE_TRANSIENT);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 18, g->op_version);
        DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 19, g->iop->params, g->iop->params_size, SQLITE_TRANSIENT);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 20, g->iop->enabled);
        DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 21, g->iop->blend_params, sizeof(dt_develop_blend_params_t),
                                   SQLITE_TRANSIENT);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 22, dt_develop_blend_version());
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 23, g->iop->multi_name, -1, SQLITE_TRANSIENT);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 24, g->iop->multi_name_hand_edited);
      }
      else
      {
        // we are in the lib case currently we set set all params to 0
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 17, g->operation, -1, SQLITE_TRANSIENT);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 18, g->op_version);
        DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 19, NULL, 0, SQLITE_TRANSIENT);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 20, 0);
        DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 21, NULL, 0, SQLITE_TRANSIENT);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 22, 0);
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 23, "", -1, SQLITE_TRANSIENT);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 24, 0);
      }
    }

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if(g->callback) ((void (*)(dt_gui_presets_edit_dialog_t *))g->callback)(g);
  }
  else if(response_id == GTK_RESPONSE_YES && g->old_id)
  {
    const gchar *name = gtk_entry_get_text(g->name);

    // ask for destination directory
    GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
          _("select directory"), GTK_WINDOW(dialog), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
          _("_select as output destination"), _("_cancel"));
    dt_conf_get_folder_to_file_chooser("ui_last/export_path", GTK_FILE_CHOOSER(filechooser));

    // save if accepted
    if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
    {
      char *filedir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
      dt_presets_save_to_file(g->old_id, name, filedir);
      dt_control_log(_("preset %s was successfully exported"), name);
      g_free(filedir);
      dt_conf_set_folder_from_file_chooser("ui_last/export_path", GTK_FILE_CHOOSER(filechooser));
    }

    g_object_unref(GTK_WIDGET(filechooser));
    return; // we don't close the window so other actions can be performed if needed
  }
  else if(response_id == GTK_RESPONSE_REJECT && g->old_id)
  {
    if(dt_gui_presets_confirm_and_delete(g->original_name, g->operation, g->old_id)
       && g->callback)
    {
      g->old_id = 0;
      ((void (*)(dt_gui_presets_edit_dialog_t *))g->callback)(g);
    }
  }

  gtk_widget_destroy(GTK_WIDGET(dialog));
  g_free(g->original_name);
  g_free(g->module_name);
  g_free(g->operation);
  free(g);
}

gboolean dt_gui_presets_confirm_and_delete(const char *name, const char *module_name, int rowid)
{
  if(!module_name) return FALSE;

  if(dt_gui_show_yes_no_dialog(_("delete preset?"),
                               _("do you really want to delete the preset `%s'?"), name))
  {
    // deregistering accel...
    for(GList *modules = darktable.iop; modules; modules = modules->next)
    {
      dt_iop_module_so_t *mod = modules->data;

      if(!strcmp(mod->op, module_name))
      {
        dt_action_rename_preset(&mod->actions, name, NULL);
        break;
      }
    }
    for(GList *libs = darktable.lib->plugins; libs; libs = g_list_next(libs))
    {
      dt_lib_module_t *lib = libs->data;

      if(!strcmp(lib->plugin_name, module_name))
      {
        dt_action_rename_preset(&lib->actions, name, NULL);
        break;
      }
    }

    // remove the preset from the database
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM data.presets WHERE rowid=?1 AND writeprotect=0", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rowid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return TRUE;
  }

  return FALSE;
}

static void _check_buttons_activated(GtkCheckButton *button, dt_gui_presets_edit_dialog_t *g)
{
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->autoapply))
     || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->filter)))
  {
    gtk_widget_set_visible(GTK_WIDGET(g->details), TRUE);
    gtk_widget_set_no_show_all(GTK_WIDGET(g->details), FALSE);
    gtk_widget_show_all(GTK_WIDGET(g->details));
    gtk_widget_set_no_show_all(GTK_WIDGET(g->details), TRUE);
  }
  else
    gtk_widget_set_visible(GTK_WIDGET(g->details), FALSE);
}

static void _presets_show_edit_dialog(dt_gui_presets_edit_dialog_t *g, gboolean allow_name_change,
                                      gboolean allow_desc_change, gboolean allow_remove)
{
  /* Create the widgets */
  char title[1024];
  snprintf(title, sizeof(title), _("edit `%s' for module `%s'"), g->original_name, g->module_name);
  GtkWidget *dialog = gtk_dialog_new_with_buttons
    (title, g->parent, GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
     _("_cancel"), GTK_RESPONSE_CANCEL, _("_export..."), GTK_RESPONSE_YES,
     _("delete"), GTK_RESPONSE_REJECT, _("_ok"), GTK_RESPONSE_OK, NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  GtkContainer *content_area = GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog)));
  GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_container_add(content_area, GTK_WIDGET(box));

  g->name = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_text(g->name, g->original_name);
  if(allow_name_change)
    gtk_entry_set_activates_default(g->name, TRUE);
  else
    gtk_widget_set_sensitive(GTK_WIDGET(g->name), FALSE);
  gtk_box_pack_start(box, GTK_WIDGET(g->name), FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->name), _("name of the preset"));

  g->description = GTK_ENTRY(gtk_entry_new());
  if(allow_desc_change)
    gtk_entry_set_activates_default(g->description, TRUE);
  else
    gtk_widget_set_sensitive(GTK_WIDGET(g->description), FALSE);
  gtk_box_pack_start(box, GTK_WIDGET(g->description), FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->description), _("description or further information"));

  g->autoapply
      = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("auto apply this preset to matching images")));
  gtk_box_pack_start(box, GTK_WIDGET(g->autoapply), FALSE, FALSE, 0);
  g->filter
      = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("only show this preset for matching images")));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->filter), _("be very careful with this option. "
                                                           "this might be the last time you see your preset."));
  gtk_box_pack_start(box, GTK_WIDGET(g->filter), FALSE, FALSE, 0);
  if(!g->iop)
  {
    // lib usually don't support autoapply
    gtk_widget_set_no_show_all(GTK_WIDGET(g->autoapply), !dt_presets_module_can_autoapply(g->module_name));
    // for libs, we don't want the filtering option as it's not implemented...
    gtk_widget_set_no_show_all(GTK_WIDGET(g->filter), TRUE);
  }
  g_signal_connect(G_OBJECT(g->autoapply), "toggled", G_CALLBACK(_check_buttons_activated), g);
  g_signal_connect(G_OBJECT(g->filter), "toggled", G_CALLBACK(_check_buttons_activated), g);

  int line = 0;
  g->details = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(g->details), DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(GTK_GRID(g->details), DT_PIXEL_APPLY_DPI(10));
  gtk_box_pack_start(box, GTK_WIDGET(g->details), TRUE, TRUE, 0);

  GtkWidget *label = NULL;

  // model, maker, lens
  g->model = gtk_entry_new();
  gtk_widget_set_hexpand(GTK_WIDGET(g->model), TRUE);
  /* xgettext:no-c-format */
  gtk_widget_set_tooltip_text(g->model, _("string to match model (use % as wildcard)"));
  label = gtk_label_new(_("model"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->model, label, GTK_POS_RIGHT, 2, 1);

  g->maker = gtk_entry_new();
  /* xgettext:no-c-format */
  gtk_widget_set_tooltip_text(g->maker, _("string to match maker (use % as wildcard)"));
  label = gtk_label_new(_("maker"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->maker, label, GTK_POS_RIGHT, 2, 1);

  g->lens = gtk_entry_new();
  /* xgettext:no-c-format */
  gtk_widget_set_tooltip_text(g->lens, _("string to match lens (use % as wildcard)"));
  label = gtk_label_new(_("lens"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->lens, label, GTK_POS_RIGHT, 2, 1);

  // iso
  label = gtk_label_new(_("ISO"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  g->iso_min = gtk_spin_button_new_with_range(0, FLT_MAX, 100);
  gtk_widget_set_tooltip_text(g->iso_min, _("minimum ISO value"));
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g->iso_min), 0);
  g->iso_max = gtk_spin_button_new_with_range(0, FLT_MAX, 100);
  gtk_widget_set_tooltip_text(g->iso_max, _("maximum ISO value"));
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g->iso_max), 0);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->iso_min, label, GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->iso_max, g->iso_min, GTK_POS_RIGHT, 1, 1);

  // exposure
  label = gtk_label_new(_("exposure"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  g->exposure_min = dt_bauhaus_combobox_new(NULL);
  g->exposure_max = dt_bauhaus_combobox_new(NULL);
  gtk_widget_set_tooltip_text(g->exposure_min, _("minimum exposure time"));
  gtk_widget_set_tooltip_text(g->exposure_max, _("maximum exposure time"));
  for(int k = 0; k < dt_gui_presets_exposure_value_cnt; k++)
    dt_bauhaus_combobox_add(g->exposure_min, dt_gui_presets_exposure_value_str[k]);
  for(int k = 0; k < dt_gui_presets_exposure_value_cnt; k++)
    dt_bauhaus_combobox_add(g->exposure_max, dt_gui_presets_exposure_value_str[k]);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->exposure_min, label, GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->exposure_max, g->exposure_min, GTK_POS_RIGHT, 1, 1);

  // aperture
  label = gtk_label_new(_("aperture"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  g->aperture_min = dt_bauhaus_combobox_new(NULL);
  g->aperture_max = dt_bauhaus_combobox_new(NULL);
  gtk_widget_set_tooltip_text(g->aperture_min, _("minimum aperture value"));
  gtk_widget_set_tooltip_text(g->aperture_max, _("maximum aperture value"));
  for(int k = 0; k < dt_gui_presets_aperture_value_cnt; k++)
    dt_bauhaus_combobox_add(g->aperture_min, dt_gui_presets_aperture_value_str[k]);
  for(int k = 0; k < dt_gui_presets_aperture_value_cnt; k++)
    dt_bauhaus_combobox_add(g->aperture_max, dt_gui_presets_aperture_value_str[k]);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->aperture_min, label, GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->aperture_max, g->aperture_min, GTK_POS_RIGHT, 1, 1);

  // focal length
  label = gtk_label_new(_("focal length"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  g->focal_length_min = gtk_spin_button_new_with_range(0, 1000, 10);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g->focal_length_min), 0);
  g->focal_length_max = gtk_spin_button_new_with_range(0, 1000, 10);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g->focal_length_max), 0);
  gtk_widget_set_tooltip_text(g->focal_length_min, _("minimum focal length"));
  gtk_widget_set_tooltip_text(g->focal_length_max, _("maximum focal length"));
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->focal_length_min, label, GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->focal_length_max, g->focal_length_min, GTK_POS_RIGHT, 1, 1);

  // raw/hdr/ldr/mono/color
  label = gtk_label_new(_("format"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line, 1, 1);
  gtk_widget_set_tooltip_text(label, _("select image types you want this preset to be available for"));

  for(int i = 0; i < 5; i++)
  {
    g->format_btn[i] = gtk_check_button_new_with_label(_(_gui_presets_format_value_str[i]));
    gtk_grid_attach(GTK_GRID(g->details), g->format_btn[i], 1, line + i, 2, 1);
  }

  gtk_widget_set_no_show_all(GTK_WIDGET(g->details), TRUE);

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "SELECT rowid, description, model, maker, lens, iso_min, iso_max, "
     "       exposure_min, exposure_max, aperture_min, aperture_max, focal_length_min, "
     "       focal_length_max, autoapply, filter, format"
     " FROM data.presets"
     " WHERE name = ?1 AND operation = ?2 AND op_version = ?3",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, g->original_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, g->operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, g->op_version);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    g->old_id = sqlite3_column_int(stmt, 0);
    gtk_entry_set_text(GTK_ENTRY(g->description), (const char *)sqlite3_column_text(stmt, 1));
    gtk_entry_set_text(GTK_ENTRY(g->model), (const char *)sqlite3_column_text(stmt, 2));
    gtk_entry_set_text(GTK_ENTRY(g->maker), (const char *)sqlite3_column_text(stmt, 3));
    gtk_entry_set_text(GTK_ENTRY(g->lens), (const char *)sqlite3_column_text(stmt, 4));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->iso_min), sqlite3_column_double(stmt, 5));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->iso_max), sqlite3_column_double(stmt, 6));

    float val = sqlite3_column_double(stmt, 7);
    int k = 0;
    for(; k < dt_gui_presets_exposure_value_cnt && val > dt_gui_presets_exposure_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->exposure_min, k);
    val = sqlite3_column_double(stmt, 8);
    for(k = 0; k < dt_gui_presets_exposure_value_cnt && val > dt_gui_presets_exposure_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->exposure_max, k);
    val = sqlite3_column_double(stmt, 9);
    for(k = 0; k < dt_gui_presets_aperture_value_cnt && val > dt_gui_presets_aperture_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->aperture_min, k);
    val = sqlite3_column_double(stmt, 10);
    for(k = 0; k < dt_gui_presets_aperture_value_cnt && val > dt_gui_presets_aperture_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->aperture_max, k);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->focal_length_min), sqlite3_column_double(stmt, 11));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->focal_length_max), sqlite3_column_double(stmt, 12));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoapply), sqlite3_column_int(stmt, 13));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->filter), sqlite3_column_int(stmt, 14));
    const int format = (sqlite3_column_int(stmt, 15)) ^ DT_PRESETS_FOR_NOT;
    for(k = 0; k < 5; k++)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->format_btn[k]), format & (_gui_presets_format_flag[k]));
  }
  else
  {
    gtk_entry_set_text(GTK_ENTRY(g->description), "");
    gtk_entry_set_text(GTK_ENTRY(g->model), "%");
    gtk_entry_set_text(GTK_ENTRY(g->maker), "%");
    gtk_entry_set_text(GTK_ENTRY(g->lens), "%");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->iso_min), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->iso_max), FLT_MAX);

    float val = 0;
    int k = 0;
    for(; k < dt_gui_presets_exposure_value_cnt && val > dt_gui_presets_exposure_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->exposure_min, k);
    val = 100000000;
    for(k = 0; k < dt_gui_presets_exposure_value_cnt && val > dt_gui_presets_exposure_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->exposure_max, k);
    val = 0;
    for(k = 0; k < dt_gui_presets_aperture_value_cnt && val > dt_gui_presets_aperture_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->aperture_min, k);
    val = 100000000;
    for(k = 0; k < dt_gui_presets_aperture_value_cnt && val > dt_gui_presets_aperture_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->aperture_max, k);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->focal_length_min), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->focal_length_max), 1000);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoapply), 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->filter), 0);
    for(k = 0; k < 5; k++) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->format_btn[k]), TRUE);
  }
  sqlite3_finalize(stmt);

  // disable remove button if needed
  if(!allow_remove || g->old_id < 0)
  {
    GtkWidget *w = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_REJECT);
    if(w) gtk_widget_set_sensitive(w, FALSE);
  }
  // disable export button if the preset is not already in the database
  if(g->old_id < 0)
  {
    GtkWidget *w = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);
    if(w) gtk_widget_set_sensitive(w, FALSE);
  }

  // put focus on cancel button if 2 first entries deactivated
  if(!allow_desc_change && !allow_name_change)
  {
    GtkWidget *w = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    if(w) gtk_widget_grab_focus(w);
  }

  g_signal_connect(dialog, "response", G_CALLBACK(_edit_preset_response), g);
  gtk_widget_show_all(dialog);
}

void dt_gui_presets_show_iop_edit_dialog(const char *name_in, dt_iop_module_t *module, GCallback final_callback,
                                         gpointer data, gboolean allow_name_change, gboolean allow_desc_change,
                                         gboolean allow_remove, GtkWindow *parent)
{
  dt_gui_presets_edit_dialog_t *g
      = (dt_gui_presets_edit_dialog_t *)g_malloc0(sizeof(dt_gui_presets_edit_dialog_t));
  g->old_id = -1;
  g->original_name = g_strdup(name_in);
  g->iop = module;
  g->operation = g_strdup(module->op);
  g->op_version = module->version();
  g->module_name = g_strdup(module->name());
  g->callback = final_callback;
  g->data = data;
  g->parent = parent;

  _presets_show_edit_dialog(g, allow_name_change, allow_desc_change, allow_remove);
}

void dt_gui_presets_show_edit_dialog(const char *name_in, const char *module_name, int rowid,
                                     GCallback final_callback, gpointer data, gboolean allow_name_change,
                                     gboolean allow_desc_change, gboolean allow_remove, GtkWindow *parent)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT operation, op_version FROM data.presets WHERE rowid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rowid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_gui_presets_edit_dialog_t *g
        = (dt_gui_presets_edit_dialog_t *)g_malloc0(sizeof(dt_gui_presets_edit_dialog_t));
    g->old_id = rowid;
    g->original_name = g_strdup(name_in);
    g->operation = g_strdup((char *)sqlite3_column_text(stmt, 0));
    g->op_version = sqlite3_column_int(stmt, 1);
    g->module_name = g_strdup(module_name);
    g->callback = final_callback;
    g->data = data;
    g->parent = parent;

    sqlite3_finalize(stmt);

    _presets_show_edit_dialog(g, allow_name_change, allow_desc_change, allow_remove);
  }
  else
    sqlite3_finalize(stmt);
}

static void _edit_preset(const char *name_in, dt_iop_module_t *module)
{
  gchar *name = NULL;
  if(name_in == NULL)
  {
    int writeprotect = -1;
    name = _get_active_preset_name(module, &writeprotect);
    if(name == NULL) return;
    if(writeprotect)
    {
      dt_control_log(_("preset `%s' is write-protected! can't edit it!"), name);
      g_free(name);
      return;
    }
  }
  else
    name = g_strdup(name_in);

  dt_gui_presets_show_iop_edit_dialog(name, module, (GCallback)_edit_preset_final_callback, NULL, TRUE, TRUE,
                                      FALSE, GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  g_free(name);
}

static void _menuitem_edit_preset(GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  _edit_preset(NULL, module);
}

static void _menuitem_update_preset(GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  gchar *name = g_object_get_data(G_OBJECT(menuitem), "dt-preset-name");

  if(!dt_conf_get_bool("plugins/lighttable/preset/ask_before_delete_preset")
     || dt_gui_show_yes_no_dialog(_("update preset?"), _("do you really want to update the preset `%s'?"), name))
  {
    // commit all the module fields
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE data.presets"
                                " SET op_version=?2, op_params=?3, enabled=?4, "
                                "     blendop_params=?5, blendop_version=?6"
                                " WHERE name=?7 AND operation=?1",
                                -1, &stmt, NULL);

    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, module->params, module->params_size, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, module->enabled);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 5, module->blend_params, sizeof(dt_develop_blend_params_t),
                               SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 6, dt_develop_blend_version());
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 7, name, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

static void _menuitem_new_preset(GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  // add new preset
  dt_lib_presets_remove(_("new preset"), module->op, module->version());

  // create a shortcut for the new entry
  dt_action_define_preset(&module->so->actions, _("new preset"));

  // then show edit dialog
  _edit_preset(_("new preset"), module);
}

void dt_gui_presets_apply_preset(const gchar* name, dt_iop_module_t *module)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
     dt_database_get(darktable.db),
     "SELECT op_params, enabled, blendop_params, blendop_version, writeprotect"
     " FROM data.presets"
     " WHERE operation = ?1 AND op_version = ?2 AND name = ?3",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, name, -1, SQLITE_TRANSIENT);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *op_params = sqlite3_column_blob(stmt, 0);
    const int op_length = sqlite3_column_bytes(stmt, 0);
    const int enabled = sqlite3_column_int(stmt, 1);
    const void *blendop_params = sqlite3_column_blob(stmt, 2);
    const int bl_length = sqlite3_column_bytes(stmt, 2);
    const int blendop_version = sqlite3_column_int(stmt, 3);
    const int writeprotect = sqlite3_column_int(stmt, 4);
    if(op_params && (op_length == module->params_size))
    {
      memcpy(module->params, op_params, op_length);
      module->enabled = enabled;
      // if module name has not been hand edited, use preset name as module label
      if(!module->multi_name_hand_edited)
        g_strlcpy(module->multi_name, name, sizeof(module->multi_name));
    }
    if(blendop_params && (blendop_version == dt_develop_blend_version())
       && (bl_length == sizeof(dt_develop_blend_params_t)))
    {
      dt_iop_commit_blend_params(module, blendop_params);
    }
    else if(blendop_params
            && dt_develop_blend_legacy_params(module, blendop_params, blendop_version, module->blend_params,
                                              dt_develop_blend_version(), bl_length) == 0)
    {
      // do nothing
    }
    else
    {
      dt_iop_commit_blend_params(module, module->default_blendop_params);
    }

    if(!writeprotect) dt_gui_store_last_preset(name);
  }
  sqlite3_finalize(stmt);
  dt_iop_gui_update(module);
  dt_dev_add_history_item(darktable.develop, module, FALSE);
  gtk_widget_queue_draw(module->widget);

  if(dt_conf_get_bool("accel/prefer_enabled") || dt_conf_get_bool("accel/prefer_unmasked"))
  {
    // rebuild the accelerators
    dt_iop_connect_accels_multi(module->so);
  }
}

void dt_gui_presets_apply_adjacent_preset(dt_iop_module_t *module, int direction)
{
  int writeprotect;
  gchar *name = _get_active_preset_name(module, &writeprotect);
  gchar *extreme = direction < 0 ? _("(first)") : _("(last)");

  sqlite3_stmt *stmt;
  // clang-format off
  gchar *query = g_strdup_printf("SELECT name"
                                 " FROM data.presets"
                                 " WHERE operation=?1 AND op_version=?2 AND"
                                 "       (?3='' OR LOWER(name) %s LOWER(?3))"
                                 " ORDER BY writeprotect %s, LOWER(name) %s"
                                 " LIMIT ?4",
                                 direction < 0 ? "<" : ">",
                                 direction < 0 ? "ASC" : "DESC",
                                 direction < 0 ? "DESC" : "ASC");
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, -1, SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, name ? name : "", -1, SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, abs(direction));
  g_free(query);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    g_free(name);
    name = g_strdup((gchar *)sqlite3_column_text(stmt, 0));
    extreme = "";
  }
  sqlite3_finalize(stmt);

  if(!*extreme)
    dt_gui_presets_apply_preset(name, module);

  dt_action_widget_toast(DT_ACTION(module), NULL, _("preset %s\n%s"),
                         extreme, name ? name : _("no presets"));
  g_free(name);
}


static void _menuitem_pick_preset(GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  gchar *name = g_object_get_data(G_OBJECT(menuitem), "dt-preset-name");
  dt_gui_presets_apply_preset(name, module);
}

gboolean dt_gui_presets_autoapply_for_module(dt_iop_module_t *module)
{
  dt_image_t *image = &module->dev->image_storage;

  const gboolean is_display_referred = dt_is_display_referred();
  const gboolean is_scene_referred = dt_is_scene_referred();
  const gboolean has_matrix = dt_image_is_matrix_correction_supported(image);

  char query[2024];
  // clang-format off
  snprintf(query, sizeof(query),
     "SELECT name"
     " FROM data.presets"
     " WHERE operation = ?1"
     "        AND ((autoapply=1"
     "           AND ((?2 LIKE model AND ?3 LIKE maker) OR (?4 LIKE model AND ?5 LIKE maker))"
     "           AND ?6 LIKE lens AND ?7 BETWEEN iso_min AND iso_max"
     "           AND ?8 BETWEEN exposure_min AND exposure_max"
     "           AND ?9 BETWEEN aperture_min AND aperture_max"
     "           AND ?10 BETWEEN focal_length_min AND focal_length_max"
     "           AND (format = 0 OR (format&?11 != 0 AND ~format&?12 != 0))"
     "           AND operation NOT IN"
     "               ('ioporder', 'metadata', 'export', 'tagging', 'collect', '%s'))"
     "  OR (name = ?13)) AND op_version = ?14",
     is_display_referred?"":"basecurve");
  // clang-format on

  sqlite3_stmt *stmt;
  const char *workflow_preset = has_matrix && is_display_referred
                                ? _("display-referred default")
                                : (has_matrix && is_scene_referred
                                   ?_("scene-referred default")
                                   :"\t\n");
  int iformat = 0;
  if(dt_image_is_rawprepare_supported(image)) iformat |= FOR_RAW;
  else iformat |= FOR_LDR;
  if(dt_image_is_hdr(image)) iformat |= FOR_HDR;

  int excluded = 0;
  if(dt_image_monochrome_flags(image)) excluded |= FOR_NOT_MONO;
  else excluded |= FOR_NOT_COLOR;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, image->exif_model, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, image->exif_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, image->camera_alias, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, image->camera_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, image->exif_lens, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, fmaxf(0.0f, fminf(FLT_MAX, image->exif_iso)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, fmaxf(0.0f, fminf(1000000, image->exif_exposure)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, fmaxf(0.0f, fminf(1000000, image->exif_aperture)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, fmaxf(0.0f, fminf(1000000, image->exif_focal_length)));
  // 0: dontcare, 1: ldr, 2: raw plus monochrome & color
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 11, iformat);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 12, excluded);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 13, workflow_preset, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 14, module->version());

  gboolean applied = FALSE;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    dt_gui_presets_apply_preset(name, module);
    applied = TRUE;
  }
  sqlite3_finalize(stmt);

  return applied;
}

static gboolean _menuitem_button_released_preset(GtkMenuItem *menuitem, GdkEventButton *event,
                                                 dt_iop_module_t *module)
{
  if(event->button == 1 || (module->flags() & IOP_FLAGS_ONE_INSTANCE))
  {
    _menuitem_pick_preset(menuitem, module);
  }
  else if(event->button == 3)
  {
    dt_iop_module_t *new_module = dt_iop_gui_duplicate(module, FALSE);
    if(new_module) _menuitem_pick_preset(menuitem, new_module);

    if(dt_conf_get_bool("darkroom/ui/rename_new_instance"))
      dt_iop_gui_rename_module(new_module);
  }

  if(dt_conf_get_bool("accel/prefer_enabled") || dt_conf_get_bool("accel/prefer_unmasked"))
  {
    // rebuild the accelerators
    dt_iop_connect_accels_multi(module->so);
  }

  return FALSE;
}

/* quick presets list
  The list of presets to show is saved in darktablerc 'plugins/darkroom/quick_preset_list' key
  the content of the key is written in the form :
    ꬹiop_name_0|preset_name_0ꬹꬹiop_name_1|preset_name_1ꬹ...
*/

static gboolean _menuitem_manage_quick_presets_traverse(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter,
                                                        gpointer data)
{
  gchar **txt = (gchar **)data;
  gchar *preset = NULL;
  gchar *iop_name = NULL;
  gboolean active = FALSE;
  gtk_tree_model_get(model, iter, 1, &active, 3, &iop_name, 4, &preset, -1);

  if(active && preset && iop_name)
  {
    *txt = dt_util_dstrcat(*txt, "ꬹ%s|%sꬹ", iop_name, preset);
  }
  g_free(iop_name);
  g_free(preset);

  return FALSE;
}

static void _menuitem_manage_quick_presets_toggle(GtkCellRendererToggle *cell_renderer, gchar *path,
                                                  gpointer tree_view)
{
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
  if(gtk_tree_model_get_iter_from_string(model, &iter, path))
  {
    if(gtk_cell_renderer_toggle_get_active(cell_renderer))
    {
      gtk_tree_store_set(GTK_TREE_STORE(model), &iter, 1, FALSE, -1);
    }
    else
    {
      gtk_tree_store_set(GTK_TREE_STORE(model), &iter, 1, TRUE, -1);
    }
  }

  // and we recreate the list of activated presets
  gchar *txt = NULL;
  gtk_tree_model_foreach(model, _menuitem_manage_quick_presets_traverse, &txt);

  dt_conf_set_string("plugins/darkroom/quick_preset_list", txt);
  g_free(txt);
}

static int _menuitem_manage_quick_presets_sort(gconstpointer a, gconstpointer b)
{
  const dt_iop_module_so_t *ma = (dt_iop_module_so_t *)a;
  const dt_iop_module_so_t *mb = (dt_iop_module_so_t *)b;
  gchar *s1 = g_utf8_normalize(ma->name(), -1, G_NORMALIZE_ALL);
  gchar *sa = g_utf8_casefold(s1, -1);
  g_free(s1);
  s1 = g_utf8_normalize(mb->name(), -1, G_NORMALIZE_ALL);
  gchar *sb = g_utf8_casefold(s1, -1);
  g_free(s1);
  const int res = g_strcmp0(sa, sb);
  g_free(sa);
  g_free(sb);
  return res;
}

static void _menuitem_manage_quick_presets(GtkMenuItem *menuitem, gpointer data)
{
  sqlite3_stmt *stmt;
  GtkWindow *win = GTK_WINDOW(dt_ui_main_window(darktable.gui->ui));
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("manage module layouts"), win,
                                                  GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, NULL, NULL);

  gtk_window_set_default_size(GTK_WINDOW(dialog), DT_PIXEL_APPLY_DPI(400), DT_PIXEL_APPLY_DPI(500));
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_set_name(dialog, "quick-presets-manager");
  gtk_window_set_title(GTK_WINDOW(dialog), _("manage quick presets"));
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

  GtkTreeViewColumn *col;
  GtkCellRenderer *renderer;
  GtkTreeModel *model;

  GtkWidget *view = gtk_tree_view_new();
  gtk_widget_set_name(view, "quick-presets-manager-list");
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), FALSE);
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), GTK_SELECTION_NONE);

  col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);

  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "markup", 0);

  col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);

  renderer = gtk_cell_renderer_toggle_new();
  g_signal_connect(renderer, "toggled", G_CALLBACK(_menuitem_manage_quick_presets_toggle), view);
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "active", 1);
  gtk_tree_view_column_add_attribute(col, renderer, "visible", 2);

  GtkTreeStore *treestore = gtk_tree_store_new(5, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING);

  gchar *config = dt_conf_get_string("plugins/darkroom/quick_preset_list");

  GList *m2 = g_list_sort(g_list_copy(darktable.iop), _menuitem_manage_quick_presets_sort);
  for(const GList *modules = m2; modules; modules = g_list_next(modules))
  {
    dt_iop_module_so_t *iop = (dt_iop_module_so_t *)modules->data;
    GtkTreeIter toplevel, child;

    /* check if module is visible in current layout */
    if(dt_dev_modulegroups_is_visible(darktable.develop, iop->op))
    {
      // create top entry
      gtk_tree_store_append(treestore, &toplevel, NULL);
      gchar *iopname = g_markup_escape_text(iop->name(), -1);
      gtk_tree_store_set(treestore, &toplevel, 0, iopname, 1, FALSE, 2, FALSE, -1);
      g_free(iopname);

      /* query presets for module */
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT name"
                                  " FROM data.presets"
                                  " WHERE operation=?1"
                                  " ORDER BY writeprotect DESC, LOWER(name), rowid",
                                  -1, &stmt, NULL);
      // clang-format on
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, iop->op, -1, SQLITE_TRANSIENT);

      int nb = 0;
      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        nb++;
        const char *name = (char *)sqlite3_column_text(stmt, 0);
        gchar *presetname = g_markup_escape_text(name, -1);
        // is this preset part of the list ?
        gchar *txt = g_strdup_printf("ꬹ%s|%sꬹ", iop->op, name);
        const gboolean inlist = (config && strstr(config, txt));
        g_free(txt);
        gtk_tree_store_append(treestore, &child, &toplevel);
        gtk_tree_store_set(treestore, &child, 0, presetname, 1, inlist, 2, TRUE, 3, iop->op, 4, name, -1);
        g_free(presetname);
      }

      sqlite3_finalize(stmt);

      // we don't show modules with no presets
      if(nb == 0) gtk_tree_store_remove(treestore, &toplevel);
    }
  }
  g_free(config);
  g_list_free(m2);

  model = GTK_TREE_MODEL(treestore);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), model);
  g_object_unref(model);

  gtk_container_add(GTK_CONTAINER(sw), view);
  gtk_widget_set_vexpand(sw, TRUE);
  gtk_widget_set_hexpand(sw, TRUE);
  gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), sw);

  gtk_window_set_resizable(GTK_WINDOW(dialog), TRUE);

  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
  gtk_widget_show_all(dialog);
}

void dt_gui_favorite_presets_menu_show()
{
  sqlite3_stmt *stmt;
  GtkMenu *menu = darktable.gui->presets_popup_menu;
  if(menu) gtk_widget_destroy(GTK_WIDGET(menu));
  darktable.gui->presets_popup_menu = GTK_MENU(gtk_menu_new());
  menu = darktable.gui->presets_popup_menu;

  const gboolean default_first = dt_conf_get_bool("plugins/darkroom/default_presets_first");

  // clang-format off
  gchar *query = g_strdup_printf("SELECT name"
                                 " FROM data.presets"
                                 " WHERE operation=?1"
                                 " ORDER BY writeprotect %s, LOWER(name), rowid",
                                 default_first ? "DESC" : "ASC"
                                );
  // clang-format on

  gboolean retrieve_list = FALSE;
  gchar *config = NULL;

  if(!dt_conf_key_exists("plugins/darkroom/quick_preset_list"))
    retrieve_list = TRUE;
  else
    config = dt_conf_get_string("plugins/darkroom/quick_preset_list");

  for(const GList *modules = g_list_last(darktable.develop->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *iop = (dt_iop_module_t *)modules->data;

    // check if module is visible in current layout
    if(dt_dev_modulegroups_is_visible(darktable.develop, iop->so->op))
    {
      /* query presets for module */
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query,
                                  -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, iop->op, -1, SQLITE_TRANSIENT);

      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        const char *name = (char *)sqlite3_column_text(stmt, 0);
        if(retrieve_list)
        {
          // we only show it if module is in favorite
          gchar *key = g_strdup_printf("plugins/darkroom/%s/favorite", iop->so->op);
          const gboolean fav = dt_conf_get_bool(key);
          g_free(key);
          if(fav) config = dt_util_dstrcat(config, "ꬹ%s|%sꬹ", iop->so->op, name);
        }

        // check that this preset is in the config list
        gchar *txt = g_strdup_printf("ꬹ%s|%sꬹ", iop->so->op, name);
        if(config && strstr(config, txt))
        {
          GtkMenuItem *mi = (GtkMenuItem *)gtk_menu_item_new_with_label(name);
          gchar *tt = g_markup_printf_escaped("<b>%s %s</b> %s", iop->name(), iop->multi_name, name);
          gtk_label_set_markup(GTK_LABEL(gtk_bin_get_child(GTK_BIN(mi))), tt);
          g_free(tt);
          g_object_set_data_full(G_OBJECT(mi), "dt-preset-name", g_strdup(name), g_free);
          g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_pick_preset), iop);
          gtk_menu_shell_append(GTK_MENU_SHELL(menu), GTK_WIDGET(mi));
        }
        g_free(txt);
      }

      sqlite3_finalize(stmt);
    }
  }
  if(retrieve_list) dt_conf_set_string("plugins/darkroom/quick_preset_list", config);
  g_free(config);
  g_free(query);

  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
  GtkMenuItem *smi_manage = (GtkMenuItem *)gtk_menu_item_new_with_label(_("manage quick presets list..."));
  g_signal_connect(G_OBJECT(smi_manage), "activate", G_CALLBACK(_menuitem_manage_quick_presets), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), GTK_WIDGET(smi_manage));
}


static void _gui_presets_popup_menu_show_internal(dt_dev_operation_t op, int32_t version,
                                                  dt_iop_params_t *params, int32_t params_size,
                                                  dt_develop_blend_params_t *bl_params, dt_iop_module_t *module,
                                                  const dt_image_t *image,
                                                  void (*pick_callback)(GtkMenuItem *, void *),
                                                  void *callback_data)
{
  GtkMenu *menu = darktable.gui->presets_popup_menu;
  if(menu) gtk_widget_destroy(GTK_WIDGET(menu));
  darktable.gui->presets_popup_menu = GTK_MENU(gtk_menu_new());
  menu = darktable.gui->presets_popup_menu;
  const gboolean hide_default = dt_conf_get_bool("plugins/darkroom/hide_default_presets");
  const gboolean default_first = dt_conf_get_bool("modules/default_presets_first");

  gchar *query = NULL;

  GtkWidget *mi;
  int active_preset = -1, cnt = 0, writeprotect = 0; //, selected_default = 0;
  sqlite3_stmt *stmt;
  // order: get shipped defaults first
  if(image)
  {
    // only matching if filter is on:
    int iformat = 0;
    if(dt_image_is_rawprepare_supported(image))
      iformat |= FOR_RAW;
    else
      iformat |= FOR_LDR;

    if(dt_image_is_hdr(image))
      iformat |= FOR_HDR;

    int excluded = 0;
    if(dt_image_monochrome_flags(image))
      excluded |= FOR_NOT_MONO;
    else
      excluded |= FOR_NOT_COLOR;

    // clang-format off
    query = g_strdup_printf
      ("SELECT name, op_params, writeprotect, description, blendop_params, "
       "  op_version, enabled"
       " FROM data.presets"
       " WHERE operation=?1"
       "   AND (filter=0"
       "          OR"
       "       (((?2 LIKE model AND ?3 LIKE maker) OR (?4 LIKE model AND ?5 LIKE maker))"
       "        AND ?6 LIKE lens"
       "        AND ?7 BETWEEN iso_min AND iso_max"
       "        AND ?8 BETWEEN exposure_min AND exposure_max"
       "        AND ?9 BETWEEN aperture_min AND aperture_max"
       "        AND ?10 BETWEEN focal_length_min AND focal_length_max"
       "        AND (format = 0 OR (format&?11 != 0 AND ~format&?12 != 0))))"
       " ORDER BY writeprotect %s, LOWER(name), rowid",
       default_first ? "DESC":"ASC");
    // clang-format on

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, op, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, image->exif_model, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, image->exif_maker, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, image->camera_alias, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, image->camera_maker, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, image->exif_lens, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, image->exif_iso);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, image->exif_exposure);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, image->exif_aperture);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, image->exif_focal_length);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 11, iformat);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 12, excluded);
  }
  else
  {
    // don't know for which image. show all we got:

    query = g_strdup_printf("SELECT name, op_params, writeprotect, "
                            "       description, blendop_params, op_version, enabled"
                            " FROM data.presets"
                            " WHERE operation=?1"
                            " ORDER BY writeprotect %s, LOWER(name), rowid",
                            default_first ? "DESC":"ASC"
                           );

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, op, -1, SQLITE_TRANSIENT);
  }
  g_free(query);
  // collect all presets for op from db
  gboolean found = 0;
  int last_wp = -1;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int chk_writeprotect = sqlite3_column_int(stmt, 2);
    if(hide_default && chk_writeprotect)
    {
      //skip default module if set to hide them.
      continue;
    }
    if(last_wp == -1)
    {
      last_wp = chk_writeprotect;
    }
    else if(last_wp != chk_writeprotect)
    {
      last_wp = chk_writeprotect;
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    }
    const void *op_params = (void *)sqlite3_column_blob(stmt, 1);
    const int32_t op_params_size = sqlite3_column_bytes(stmt, 1);
    const void *blendop_params = (void *)sqlite3_column_blob(stmt, 4);
    const int32_t bl_params_size = sqlite3_column_bytes(stmt, 4);
    const int32_t preset_version = sqlite3_column_int(stmt, 5);
    const int32_t enabled = sqlite3_column_int(stmt, 6);
    const int32_t isdisabled = (preset_version == version ? 0 : 1);
    const char *name = (char *)sqlite3_column_text(stmt, 0);
    gboolean isdefault = FALSE;

    if(darktable.gui->last_preset && strcmp(darktable.gui->last_preset, name) == 0)
      found = TRUE;

    if(module
       && !memcmp(module->default_params, op_params,
                  MIN(op_params_size, module->params_size))
       && !memcmp(module->default_blendop_params, blendop_params,
                  MIN(bl_params_size, sizeof(dt_develop_blend_params_t))))
      isdefault = TRUE;

    gchar *label;
    if(isdefault)
      label = g_strdup_printf("%s %s", name, _("(default)"));
    else
      label = g_strdup(name);
    mi = gtk_check_menu_item_new_with_label(label);
    dt_gui_add_class(mi, "dt_transparent_background");
    g_free(label);

    if(module
       && !memcmp(params, op_params, MIN(op_params_size, params_size))
       && !memcmp(bl_params, blendop_params, MIN(bl_params_size, sizeof(dt_develop_blend_params_t)))
       && module->enabled == enabled)
    {
      active_preset = cnt;
      writeprotect = sqlite3_column_int(stmt, 2);
      dt_gui_add_class(mi, "active_menu_item");
      gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), TRUE);
    }

    if(isdisabled)
    {
      gtk_widget_set_sensitive(mi, 0);
      gtk_widget_set_tooltip_text(mi, _("disabled: wrong module version"));
    }
    else
    {
      g_object_set_data_full(G_OBJECT(mi), "dt-preset-name", g_strdup(name), g_free);
      if(module)
      {
        g_signal_connect(G_OBJECT(mi), "button-release-event", G_CALLBACK(_menuitem_button_released_preset),
                         module);
      }
      else if(pick_callback)
        g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(pick_callback), callback_data);
      gtk_widget_set_tooltip_text(mi, (const char *)sqlite3_column_text(stmt, 3));
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    cnt++;
  }
  sqlite3_finalize(stmt);

  if(cnt > 0) gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

  if(module)
  {
    if(active_preset >= 0 && !writeprotect)
    {
      mi = gtk_menu_item_new_with_label(_("edit this preset.."));
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_edit_preset), module);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

      mi = gtk_menu_item_new_with_label(_("delete this preset"));
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_delete_preset), module);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    else
    {
      mi = gtk_menu_item_new_with_label(_("store new preset.."));
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_new_preset), module);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

      if(darktable.gui->last_preset && found)
      {
        char *markup = g_markup_printf_escaped("%s <span weight='bold'>%s</span>", _("update preset"),
                                               darktable.gui->last_preset);
        mi = gtk_menu_item_new_with_label("");
        gtk_label_set_markup(GTK_LABEL(gtk_bin_get_child(GTK_BIN(mi))), markup);
        g_object_set_data_full(G_OBJECT(mi), "dt-preset-name", g_strdup(darktable.gui->last_preset), g_free);
        g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_update_preset), module);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        g_free(markup);
      }
    }
  }

  // and the parameters entry if needed
  if(module && (module->set_preferences || module->flags() & IOP_FLAGS_GUIDES_WIDGET))
  {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    // the guide checkbox
    if(module->flags() & IOP_FLAGS_GUIDES_WIDGET) dt_guides_add_module_menuitem(menu, module);
    // the specific parameters
    if(module->set_preferences) module->set_preferences(GTK_MENU_SHELL(menu), module);
  }
}

void dt_gui_presets_popup_menu_show_for_params(dt_dev_operation_t op, int32_t version, void *params,
                                               int32_t params_size, void *blendop_params,
                                               const dt_image_t *image,
                                               void (*pick_callback)(GtkMenuItem *, void *),
                                               void *callback_data)
{
  _gui_presets_popup_menu_show_internal(op, version, params, params_size, blendop_params, NULL, image,
                                        pick_callback, callback_data);
}

void dt_gui_presets_popup_menu_show_for_module(dt_iop_module_t *module)
{
  _gui_presets_popup_menu_show_internal(module->op, module->version(), module->params, module->params_size,
                                        module->blend_params, module, &module->dev->image_storage, NULL, NULL);
}

void dt_gui_presets_update_mml(const char *name, dt_dev_operation_t op, const int32_t version,
                               const char *maker, const char *model, const char *lens)
{
  sqlite3_stmt *stmt;
  // clang-format off¨
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "UPDATE data.presets"
      " SET maker='%' || ?1 || '%', model=?2, lens=?3"
      " WHERE operation=?4 AND op_version=?5 AND name=?6", -1,
      &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, maker, -1, SQLITE_TRANSIENT);
  if(*model)
  {
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, model, -1, SQLITE_TRANSIENT);
  }
  else
  {
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, "%", -1, SQLITE_TRANSIENT);
  }
  if(*lens)
  {
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, lens, -1, SQLITE_TRANSIENT);
  }
  else
  {
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, "%", -1, SQLITE_TRANSIENT);
  }
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_gui_presets_update_iso(const char *name, dt_dev_operation_t op, const int32_t version,
                               const float min, const float max)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "UPDATE data.presets"
      " SET iso_min=?1, iso_max=?2"
      " WHERE operation=?3 AND op_version=?4 AND name=?5", -1, &stmt,
      NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 1, min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 2, max);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_gui_presets_update_av(const char *name, dt_dev_operation_t op, const int32_t version, const float min,
                              const float max)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "UPDATE data.presets"
      " SET aperture_min=?1, aperture_max=?2"
      " WHERE operation=?3 AND op_version=?4 AND name=?5",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 1, min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 2, max);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_gui_presets_update_tv(const char *name, dt_dev_operation_t op, const int32_t version, const float min,
                              const float max)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "UPDATE data.presets SET exposure_min=?1, exposure_max=?2 WHERE operation=?3 AND op_version=?4 AND name=?5",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 1, min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 2, max);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_gui_presets_update_fl(const char *name, dt_dev_operation_t op, const int32_t version, const float min,
                              const float max)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.presets"
                              " SET focal_length_min=?1, focal_length_max=?2"
                              " WHERE operation=?3 AND op_version=?4 AND name=?5",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 1, min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 2, max);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_gui_presets_update_ldr(const char *name, dt_dev_operation_t op, const int32_t version,
                               const int ldrflag)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.presets"
                              " SET format=?1"
                              " WHERE operation=?2 AND op_version=?3 AND name=?4",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, ldrflag);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_gui_presets_update_autoapply(const char *name, dt_dev_operation_t op, const int32_t version,
                                     const int autoapply)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "UPDATE data.presets"
      " SET autoapply=?1"
      " WHERE operation=?2 AND op_version=?3 AND name=?4", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, autoapply);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_gui_presets_update_filter(const char *name, dt_dev_operation_t op, const int32_t version,
                                  const int filter)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.presets"
                              " SET filter=?1"
                              " WHERE operation=?2 AND op_version=?3 AND name=?4",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filter);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
