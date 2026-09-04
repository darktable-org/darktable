/*
    This file is part of darktable,
    Copyright (C) 2010-2026 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/presets.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/guides.h"
#include "gui/presets.h"
#include <glib-2.0/gio/gio.h>

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <stdlib.h>

#define MAX_FOCAL_LEN 100000

const int dt_gui_presets_exposure_value_cnt = 22;
const float dt_gui_presets_exposure_value[]
    = { 0.,     1./8000, 1./4000, 1./2000, 1./1000, 1./500, 1./250,
        1./125, 1./60,   1./30,   1./15,   1./8,    1./4,   1./2,
        1,      2,       4,       8,       15,      30,     60,     FLT_MAX };
const char *dt_gui_presets_exposure_value_str[]
    = { "0",     "1/8000", "1/4000", "1/2000", "1/1000", "1/500", "1/250",
        "1/125", "1/60",   "1/30",   "1/15",   "1/8",    "1/4",   "1/2",
        "1\"",   "2\"",    "4\"",    "8\"",    "15\"",   "30\"",  "60\"",  "+" };

const int dt_gui_presets_aperture_value_cnt = 21;
const float dt_gui_presets_aperture_value[]
    = { 0,   0.95,  1.0,  1.2,  1.4,  1.8,  2.0,  2.4,   2.8,  4.0,   5.6,
        8.0, 11.0, 16.0, 22.0, 32.0, 45.0, 64.0, 90.0, 128.0, FLT_MAX };
const char *dt_gui_presets_aperture_value_str[]
    = { "f/0", "f/0.95", "f/1.0", "f/1.2", "f/1.4", "f/1.8",  "f/2", "f/2.4", "f/2.8", "f/4", "f/5.6",
        "f/8",   "f/11",  "f/16",  "f/22",  "f/32",  "f/45", "f/64",  "f/90", "f/128", "f/+" };

// format string and corresponding flag stored into the database
static const char *_gui_presets_format_value_str[5]
    = { N_("non-raw"), N_("raw"), N_("HDR"), N_("monochrome"), N_("color") };
static const int _gui_presets_format_flag[5] =
  { FOR_LDR, FOR_RAW, FOR_HDR, FOR_NOT_MONO, FOR_NOT_COLOR };

void _insert_text_event(GtkEditable *editable,
                        const gchar *text,
                        const gint length,
                        gint *position,
                        gpointer data)
{
  for(int i = 0; i < length; i++)
  {
    if(!g_ascii_isdigit(text[i]))
    {
      g_signal_stop_emission_by_name(G_OBJECT(editable), "insert-text");
      return;
    }
  }
}

// this is also called for non-gui applications linking to
// libdarktable!  so beware, don't use any darktable.gui stuff here
// .. (or change this behaviour in darktable.c)
void dt_gui_presets_init()
{
  // remove auto generated presets from plugins, not the user included
  // ones.
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "DELETE FROM data.presets WHERE writeprotect = 1", NULL,
                        NULL, NULL);
}

void (dt_gui_presets_add_generic)(const char *name,
                                  const dt_dev_operation_t op,
                                  const int32_t version,
                                  const void *params,
                                  const int32_t params_size,
                                  const gboolean enabled,
                                  const dt_develop_blend_colorspace_t blend_cst)
{
  dt_develop_blend_params_t default_blendop_params;
  dt_develop_blend_init_blend_parameters(&default_blendop_params, blend_cst);
  gchar *prefixed_name = g_strdup_printf(BUILTIN_PREFIX "%s", name);
  dt_gui_presets_add_with_blendop(prefixed_name, op, version, params, params_size,
                                  &default_blendop_params, enabled);
  g_free(prefixed_name);
}

void dt_gui_presets_add_with_blendop(const char *name,
                                     const dt_dev_operation_t op,
                                     const int32_t version,
                                     const void *params,
                                     const int32_t params_size,
                                     const void *blend_params,
                                     const gboolean enabled)
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
      "                    writeprotect, autoapply, filter, def, format, multi_name_hand_edited)"
      " VALUES (?1, '', ?2, ?3, ?4, ?5, ?6, ?7, 0, '', '%', '%', '%', 0,"
      "         340282346638528859812000000000000000000, 0, 10000000, 0, 100000000, 0,"
      "         ?8, 1, 0, 0, 0, 0, 0)",
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
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 8, MAX_FOCAL_LEN);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static void _menuitem_delete_preset(GSimpleAction *action,
                                    GVariant *parameter,
                                    gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  gboolean writeprotect = FALSE;
  gchar *name = dt_get_active_preset_name(module, &writeprotect);
  if(name == NULL) return;

  if(writeprotect)
  {
    dt_control_log(_("preset `%s' is write-protected, can't delete!"), name);
    g_free(name);
    return;
  }

  if(!dt_conf_get_bool("plugins/lighttable/preset/ask_before_delete_preset")
     || dt_gui_show_yes_no_dialog(_("delete preset?"), "",
                                  _("do you really want to delete the preset `%s'?"), name))
  {
    dt_action_rename_preset(&module->so->actions, name, NULL);

    dt_lib_presets_remove(name, module->op, module->version());

    dt_iop_update_multi_name(module, "", module->multi_name_hand_edited, FALSE, FALSE);
  }
  g_free(name);
}

static void _edit_preset_final_callback(dt_gui_presets_edit_dialog_t *g)
{
  const char *name = gtk_entry_get_text(g->name);

  dt_iop_update_multi_name(g->iop, name, g->iop->multi_name_hand_edited, FALSE, FALSE);

  dt_gui_store_last_preset(name);
}

static void _edit_preset_response(GtkDialog *dialog,
                                  const gint response_id,
                                  dt_gui_presets_edit_dialog_t *g)
{
  if(response_id == GTK_RESPONSE_OK)
  {
    // find the module action list this preset belongs to
    dt_action_t *module_actions = g->iop ? &g->iop->so->actions : NULL;

    for(GList *libs = darktable.lib->plugins;
        !module_actions && libs;
        libs = g_list_next(libs))
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
        GtkWidget *dlg_changename =
          gtk_message_dialog_new(GTK_WINDOW(dialog),
                                 GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                 GTK_MESSAGE_WARNING,
                                 GTK_BUTTONS_OK, _("please give preset a name"));
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

        // if result is BUTTON_NO or ESCAPE keypress exit without
        // destroying dialog, to permit other name
        if(dt_gui_show_yes_no_dialog
           (_("overwrite preset?"), "",
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

    //
    // g->iop    : if set we are editing an iop preset
    // g->old_id : if > 0 we are modifiing an existing preset
    //

    gchar *query = NULL;
    if(g->old_id >= 0)
    {
      // we update presets values
      // clang-format off
      query = g_strdup_printf
        ("UPDATE data.presets "
         "SET"
         "  name=?1, description=?2,"
         "  model=?3, maker=?4, lens=?5, iso_min=?6, iso_max=?7, exposure_min=?8,"
         "  exposure_max=?9, aperture_min=?10,"
         "  aperture_max=?11, focal_length_min=?12, focal_length_max=?13, autoapply=?14,"
         "  filter=?15, format=?16 %s"
         " WHERE rowid=%d",
         g->iop
           ? ", op_params=?19, enabled=?20, multi_name=?23, multi_name_hand_edited=?24"
           : "",
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
         "  multi_priority, multi_name, multi_name_hand_edited) "
         "VALUES"
         " (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16,"
         "   0, 0, ?17, ?18, ?19, ?20, ?21, ?22, 0, ?23, ?24)");
      // clang-format on
    }

    // rename accelerators
    dt_action_rename_preset(module_actions, g->original_name, name);

    // commit all the user input fields
    const gboolean is_auto_init =
      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->autoinit));

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    g_free(query);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, gtk_entry_get_text(g->description),
                               -1, SQLITE_TRANSIENT);

    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, gtk_entry_get_text(GTK_ENTRY(g->model)),
                               -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, gtk_entry_get_text(GTK_ENTRY(g->maker)),
                               -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, gtk_entry_get_text(GTK_ENTRY(g->lens)),
                               -1, SQLITE_TRANSIENT);

    const gchar *iso_min_entered_text = gtk_entry_get_text(GTK_ENTRY(g->iso_min));
    if(iso_min_entered_text[0] == '\0') iso_min_entered_text = "0";
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 6, atof(iso_min_entered_text));

    const gchar *iso_max_entered_text = gtk_entry_get_text(GTK_ENTRY(g->iso_max));
    // We want FLT_MAX value in the database when iso_max field was empty.
    if(iso_max_entered_text[0] == '\0')
    {
      DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, FLT_MAX);
    }
    else
    {
      DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, atof(iso_max_entered_text));
    }

    DT_DEBUG_SQLITE3_BIND_DOUBLE
      (stmt, 8,
       dt_gui_presets_exposure_value[dt_bauhaus_combobox_get(g->exposure_min)]);
    DT_DEBUG_SQLITE3_BIND_DOUBLE
      (stmt, 9,
       dt_gui_presets_exposure_value[dt_bauhaus_combobox_get(g->exposure_max)]);
    DT_DEBUG_SQLITE3_BIND_DOUBLE
      (stmt, 10,
       dt_gui_presets_aperture_value[dt_bauhaus_combobox_get(g->aperture_min)]);
    DT_DEBUG_SQLITE3_BIND_DOUBLE
      (stmt, 11,
       dt_gui_presets_aperture_value[dt_bauhaus_combobox_get(g->aperture_max)]);
    DT_DEBUG_SQLITE3_BIND_DOUBLE
      (stmt, 12,
       gtk_spin_button_get_value(GTK_SPIN_BUTTON(g->focal_length_min)));
    DT_DEBUG_SQLITE3_BIND_DOUBLE
      (stmt, 13,
       gtk_spin_button_get_value(GTK_SPIN_BUTTON(g->focal_length_max)));
    DT_DEBUG_SQLITE3_BIND_INT
      (stmt, 14,
       gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->autoapply)));
    DT_DEBUG_SQLITE3_BIND_INT
      (stmt, 15,
       gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->filter)));

    int format = 0;
    for(int k = 0; k < 5; k++)
      format += gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->format_btn[k]))
        * _gui_presets_format_flag[k];

    format ^= DT_PRESETS_FOR_NOT;

    DT_DEBUG_SQLITE3_BIND_INT(stmt, 16, format);

    // for a new preset or one that is for an iop module
    if(g->iop)
    {
      // for auto init presets we don't record the params. When applying such preset
      // the default params will be used and this will trigger the computation of
      // the actual parameters.
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 19,
                                 is_auto_init ? NULL : g->iop->params,
                                 is_auto_init ?    0 : g->iop->params_size,
                                 SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 20, g->iop->enabled);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 23, g->iop->multi_name_hand_edited
                                 ? g->iop->multi_name
                                 : name,
                                 -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 24, g->iop->multi_name_hand_edited);
    }

    // commit specific fields in case of newly created preset
    if(g->old_id < 0)
    {
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 17, g->operation, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 18, g->op_version);

      if(g->iop)
      {
        DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 21, g->iop->blend_params,
                                   sizeof(dt_develop_blend_params_t), SQLITE_TRANSIENT);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 22, dt_develop_blend_version());
      }
      else
      {
        // we are in the lib case currently we set set all params to 0
        DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 21, NULL, 0, SQLITE_TRANSIENT);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 22, 0);
      }
    }

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if(g->callback) ((void (*)(dt_gui_presets_edit_dialog_t *))g->callback)(g);
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_PRESETS_CHANGED,
                            g_strdup(g->operation));
  }
  else if(response_id == GTK_RESPONSE_YES && g->old_id)
  {
    const gchar *name = gtk_entry_get_text(g->name);

    // ask for destination directory
    GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
          _("select directory"), GTK_WINDOW(dialog), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
          _("_select as output destination"), _("_cancel"));
    dt_conf_get_folder_to_file_chooser("ui_last/export_path",
                                       GTK_FILE_CHOOSER(filechooser));

    // save if accepted
    if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
    {
      char *filedir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
      dt_presets_save_to_file(g->old_id, name, filedir);
      dt_control_log(_("preset %s was successfully exported"), name);
      g_free(filedir);
      dt_conf_set_folder_from_file_chooser("ui_last/export_path",
                                           GTK_FILE_CHOOSER(filechooser));
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

gboolean dt_gui_presets_confirm_and_delete(const char *name,
                                           const char *module_name,
                                           const int rowid)
{
  if(!module_name) return FALSE;

  if(dt_gui_show_yes_no_dialog(_("delete preset?"), "",
                               _("do you really want to delete the preset `%s'?"), name))
  {
    // deregistering accel...
    for(GList *modules = darktable.iop; modules; modules = modules->next)
    {
      dt_iop_module_so_t *mod_so = modules->data;
      if(dt_iop_module_so_is(mod_so, module_name))
      {
        dt_action_rename_preset(&mod_so->actions, name, NULL);
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
                                "DELETE FROM data.presets"
                                " WHERE rowid=?1 AND writeprotect=0", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rowid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return TRUE;
  }

  return FALSE;
}

static void _check_buttons_activated(GtkCheckButton *button,
                                     dt_gui_presets_edit_dialog_t *g)
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

static void _format_toggled(GtkToggleButton *button, gpointer data)
{
  dt_gui_presets_edit_dialog_t *g = (dt_gui_presets_edit_dialog_t *)data;

  GtkWidget *ok_button =
    gtk_dialog_get_widget_for_response((GtkDialog *)g->dialog, GTK_RESPONSE_OK);

  // active if one of first group (raw, non-raw) selected and one on the
  // second group (hdr, color, monochrome).

  const gboolean raw_col =
    gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->format_btn[0]))
    || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->format_btn[1]));

  const gboolean kind_col =
    gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->format_btn[2]))
    || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->format_btn[3]))
    || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->format_btn[4]));

  const gboolean ok_active = !g->iop || (raw_col && kind_col);

  // second column visible only if at least one item selected in first
  // column.

  for(int k=2; k<5; k++)
    gtk_widget_set_visible(g->format_btn[k], raw_col);

  // "and" label sensitive only if at least one item selected in first
  // column.
  gtk_widget_set_sensitive(g->and_label, raw_col);

  gtk_widget_set_sensitive(ok_button, ok_active);
}

static void _presets_show_edit_dialog(dt_gui_presets_edit_dialog_t *g,
                                      const gboolean allow_name_change,
                                      const gboolean allow_desc_change,
                                      const gboolean allow_remove)
{
  /* Create the widgets */
  const char *lname = dt_util_localize_string(g->module_name);
  char title[1024];
  snprintf(title, sizeof(title), _("edit `%s' for module `%s'"),
           g->original_name, lname);

  GtkWidget *dialog = gtk_dialog_new_with_buttons(title, g->parent,
                                                  GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                                  _("_export..."), GTK_RESPONSE_YES,
                                                  _("_delete"), GTK_RESPONSE_REJECT,
                                                  _("_cancel"), GTK_RESPONSE_CANCEL,
                                                  _("_ok"), GTK_RESPONSE_OK, NULL);
  dt_gui_dialog_add_help(GTK_DIALOG(dialog), "preset_dialog");
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

  g->dialog = dialog;

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  g->name = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_text(g->name, g->original_name);
  gtk_entry_set_width_chars(g->name, 10 + g_utf8_strlen(title, -1));
  if(allow_name_change)
    gtk_entry_set_activates_default(g->name, TRUE);
  else
    gtk_widget_set_sensitive(GTK_WIDGET(g->name), FALSE);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->name), _("name of the preset"));

  g->description = GTK_ENTRY(gtk_entry_new());
  if(allow_desc_change)
    gtk_entry_set_activates_default(g->description, TRUE);
  else
    gtk_widget_set_sensitive(GTK_WIDGET(g->description), FALSE);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->description),
                              _("description or further information"));

  g->autoinit
      = GTK_CHECK_BUTTON(gtk_check_button_new_with_label
                         (_("reset all module parameters to their default values")));
  gtk_widget_set_tooltip_text
    (GTK_WIDGET(g->autoinit),
     _("the parameters will be reset to their default values,"
       " which may be automatically set based on image metadata"));

  g->autoapply
      = GTK_CHECK_BUTTON(gtk_check_button_new_with_label
                         (_("auto apply this preset to matching images")));
  g->filter
      = GTK_CHECK_BUTTON(gtk_check_button_new_with_label
                         (_("only show this preset for matching images")));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->filter),
                              _("be very careful with this option. "
                                "this might be the last time you see your preset."));

  // check if module_name is an IOP module
  const dt_iop_module_so_t *module = dt_iop_get_module_so(g->operation);

  if(!module)
  {
    // lib usually don't support auto-init / autoapply
    gtk_widget_set_no_show_all(GTK_WIDGET(g->autoinit), TRUE);
    gtk_widget_set_no_show_all(GTK_WIDGET(g->autoapply),
                               !dt_presets_module_can_autoapply(g->operation));
    // for libs, we don't want the filtering option as it's not implemented...
    gtk_widget_set_no_show_all(GTK_WIDGET(g->filter), TRUE);
  }
  else
  {
    // without an IOP history we cannot support autoinit
    gtk_widget_set_sensitive(GTK_WIDGET(g->autoinit), g->iop != NULL);
    gtk_widget_set_sensitive(GTK_WIDGET(g->filter), TRUE);
  }

  g_signal_connect(G_OBJECT(g->autoapply), "toggled",
                   G_CALLBACK(_check_buttons_activated), g);
  g_signal_connect(G_OBJECT(g->filter), "toggled",
                   G_CALLBACK(_check_buttons_activated), g);

  int line = 0;
  g->details = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(g->details), DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(GTK_GRID(g->details), DT_PIXEL_APPLY_DPI(10));
  gtk_grid_set_row_homogeneous(GTK_GRID(g->details), TRUE);

  GtkWidget *label = NULL;

  // model, maker, lens
  g->model = gtk_entry_new();
  gtk_widget_set_hexpand(GTK_WIDGET(g->model), TRUE);
  /* xgettext:no-c-format */
  gtk_widget_set_tooltip_text(g->model, _("string to match model (use % as wildcard)"));
  label = gtk_label_new(_("model"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->model, label, GTK_POS_RIGHT, 4, 1);

  g->maker = gtk_entry_new();
  /* xgettext:no-c-format */
  gtk_widget_set_tooltip_text(g->maker, _("string to match maker (use % as wildcard)"));
  label = gtk_label_new(_("maker"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->maker, label, GTK_POS_RIGHT, 4, 1);

  g->lens = gtk_entry_new();
  /* xgettext:no-c-format */
  gtk_widget_set_tooltip_text(g->lens, _("string to match lens (use % as wildcard)"));
  label = gtk_label_new(_("lens"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->lens, label, GTK_POS_RIGHT, 4, 1);

  // iso
  label = gtk_label_new(_("ISO"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  g->iso_min = gtk_entry_new();
  gtk_widget_set_tooltip_text(g->iso_min, _("minimum ISO value"));
  g_signal_connect(G_OBJECT(g->iso_min), "insert-text",
                   G_CALLBACK(_insert_text_event), NULL);
  g->iso_max = gtk_entry_new();
  gtk_widget_set_tooltip_text
    (g->iso_max,
     _("maximum ISO value\nif left blank, it is equivalent to no upper limit"));
  g_signal_connect(G_OBJECT(g->iso_max), "insert-text",
                   G_CALLBACK(_insert_text_event), NULL);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->iso_min, label, GTK_POS_RIGHT, 2, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->iso_max, g->iso_min,
                          GTK_POS_RIGHT, 2, 1);

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
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->exposure_min, label,
                          GTK_POS_RIGHT, 2, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->exposure_max, g->exposure_min,
                          GTK_POS_RIGHT, 2, 1);

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
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->aperture_min, label,
                          GTK_POS_RIGHT, 2, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->aperture_max, g->aperture_min,
                          GTK_POS_RIGHT, 2, 1);

  // focal length
  label = gtk_label_new(_("focal length"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  g->focal_length_min = gtk_spin_button_new_with_range(0, MAX_FOCAL_LEN, 10);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g->focal_length_min), 0);
  g->focal_length_max = gtk_spin_button_new_with_range(0, MAX_FOCAL_LEN, 10);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g->focal_length_max), 0);
  gtk_widget_set_tooltip_text(g->focal_length_min, _("minimum focal length"));
  gtk_widget_set_tooltip_text(g->focal_length_max, _("maximum focal length"));
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->focal_length_min, label,
                          GTK_POS_RIGHT, 2, 1);
  gtk_grid_attach_next_to(GTK_GRID(g->details), g->focal_length_max, g->focal_length_min,
                          GTK_POS_RIGHT, 2, 1);

  // raw/hdr/ldr/mono/color
  label = gtk_label_new(_("format"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(g->details), label, 0, line, 1, 1);
  gtk_widget_set_tooltip_text
    (label,
     _("select image types you want this preset to be available for"));

  for(int i = 0; i < 5; i++)
  {
    g->format_btn[i] =
      gtk_check_button_new_with_label(_(_gui_presets_format_value_str[i]));
    g_signal_connect(g->format_btn[i], "toggled", G_CALLBACK(_format_toggled), g);
  }

  // raw / non-raw
  gtk_grid_attach(GTK_GRID(g->details), g->format_btn[0], 1, line + 0, 1, 1);
  gtk_grid_attach(GTK_GRID(g->details), g->format_btn[1], 1, line + 2, 1, 1);

  g->and_label = gtk_label_new(_("and"));
  gtk_widget_set_halign(g->and_label, GTK_ALIGN_CENTER);
  gtk_grid_attach(GTK_GRID(g->details), g->and_label, 2, line + 1, 1, 1);

  // hdr/mono/color
  gtk_grid_attach(GTK_GRID(g->details), g->format_btn[2], 4, line + 0, 1, 1);
  gtk_grid_attach(GTK_GRID(g->details), g->format_btn[3], 4, line + 1, 1, 1);
  gtk_grid_attach(GTK_GRID(g->details), g->format_btn[4], 4, line + 2, 1, 1);

  gtk_widget_set_no_show_all(GTK_WIDGET(g->details), TRUE);

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "SELECT rowid, description, model, maker, lens, iso_min, iso_max, "
     "       exposure_min, exposure_max, aperture_min, aperture_max, focal_length_min, "
     "       focal_length_max, autoapply, filter, format, op_params"
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
    gtk_entry_set_text(GTK_ENTRY(g->description),
                       (const char *)sqlite3_column_text(stmt, 1));
    gtk_entry_set_text(GTK_ENTRY(g->model), (const char *)sqlite3_column_text(stmt, 2));
    gtk_entry_set_text(GTK_ENTRY(g->maker), (const char *)sqlite3_column_text(stmt, 3));
    gtk_entry_set_text(GTK_ENTRY(g->lens), (const char *)sqlite3_column_text(stmt, 4));

    char *iso_min_fromdb = (char *)sqlite3_column_text(stmt, 5);
    char *iso_max_fromdb = (char *)sqlite3_column_text(stmt, 6);

    gtk_entry_set_text(GTK_ENTRY(g->iso_min), strtok(iso_min_fromdb,"."));

    // A simple way to check if FLT_MAX has been written to the database is to check if
    // there is "e+38" in the text representation of the read value.
    if(g_str_has_suffix(iso_max_fromdb,"e+38"))
    {
      gtk_entry_set_placeholder_text(GTK_ENTRY(g->iso_max), _("∞"));
    }
    else
    {
      gtk_entry_set_text(GTK_ENTRY(g->iso_max), strtok(iso_max_fromdb,"."));
    }

    float val = sqlite3_column_double(stmt, 7);
    int k = 0;
    for(; k < dt_gui_presets_exposure_value_cnt
          && val > dt_gui_presets_exposure_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->exposure_min, k);
    val = sqlite3_column_double(stmt, 8);
    for(k = 0; k < dt_gui_presets_exposure_value_cnt
          && val > dt_gui_presets_exposure_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->exposure_max, k);
    val = sqlite3_column_double(stmt, 9);
    for(k = 0; k < dt_gui_presets_aperture_value_cnt
          && val > dt_gui_presets_aperture_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->aperture_min, k);
    val = sqlite3_column_double(stmt, 10);
    for(k = 0; k < dt_gui_presets_aperture_value_cnt
          && val > dt_gui_presets_aperture_value[k]; k++)
      ;
    dt_bauhaus_combobox_set(g->aperture_max, k);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->focal_length_min),
                              sqlite3_column_double(stmt, 11));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->focal_length_max),
                              sqlite3_column_double(stmt, 12));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoapply),
                                 sqlite3_column_int(stmt, 13));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->filter),
                                 sqlite3_column_int(stmt, 14));
    const int format = (sqlite3_column_int(stmt, 15)) ^ DT_PRESETS_FOR_NOT;
    for(k = 0; k < 5; k++)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->format_btn[k]),
                                   format & (_gui_presets_format_flag[k]));

    const int op_params_length = sqlite3_column_bytes(stmt, 16);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoinit),
                                 (op_params_length == 0));
  }
  else
  {
    gtk_entry_set_text(GTK_ENTRY(g->description), "");
    gtk_entry_set_text(GTK_ENTRY(g->model), "%");
    gtk_entry_set_text(GTK_ENTRY(g->maker), "%");
    gtk_entry_set_text(GTK_ENTRY(g->lens), "%");
    gtk_entry_set_text(GTK_ENTRY(g->iso_min), "0");
    gtk_entry_set_placeholder_text(GTK_ENTRY(g->iso_max), _("∞"));

    dt_bauhaus_combobox_set(g->exposure_min, 0);
    dt_bauhaus_combobox_set(g->exposure_max, dt_gui_presets_exposure_value_cnt-1);
    dt_bauhaus_combobox_set(g->aperture_min, 0);
    dt_bauhaus_combobox_set(g->aperture_max, dt_gui_presets_aperture_value_cnt-1);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->focal_length_min), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->focal_length_max), MAX_FOCAL_LEN);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoapply), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->filter), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoinit), FALSE);

    for(int k = 0; k < 5; k++)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->format_btn[k]), TRUE);
  }
  sqlite3_finalize(stmt);

  // disable remove button if needed
  if(!allow_remove || g->old_id < 0)
  {
    GtkWidget *w = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),
                                                      GTK_RESPONSE_REJECT);
    if(w) gtk_widget_set_sensitive(w, FALSE);
  }
  // disable export button if the preset is not already in the database
  if(g->old_id < 0)
  {
    GtkWidget *w = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),
                                                      GTK_RESPONSE_YES);
    if(w) gtk_widget_set_sensitive(w, FALSE);
  }

  // put focus on cancel button if 2 first entries deactivated
  if(!allow_desc_change && !allow_name_change)
  {
    GtkWidget *w = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),
                                                      GTK_RESPONSE_CANCEL);
    if(w) gtk_widget_grab_focus(w);
  }

  dt_gui_dialog_add(GTK_DIALOG(dialog), g->name, g->description,
                    g->autoinit, g->autoapply, g->filter, g->details);

  g_signal_connect(dialog, "response", G_CALLBACK(_edit_preset_response), g);
  gtk_widget_show_all(dialog);
}

void dt_gui_presets_show_iop_edit_dialog(const char *name_in,
                                         dt_iop_module_t *module,
                                         GCallback final_callback,
                                         gpointer data,
                                         const gboolean allow_name_change,
                                         const gboolean allow_desc_change,
                                         const gboolean allow_remove,
                                         GtkWindow *parent)
{
  dt_gui_presets_edit_dialog_t *g = g_malloc0(sizeof(dt_gui_presets_edit_dialog_t));
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

void dt_gui_presets_show_edit_dialog(const char *name_in,
                                     int rowid,
                                     GCallback final_callback,
                                     gpointer data,
                                     const gboolean allow_name_change,
                                     const gboolean allow_desc_change,
                                     const gboolean allow_remove,
                                     GtkWindow *parent)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT operation, op_version"
                              " FROM data.presets"
                              " WHERE rowid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rowid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_gui_presets_edit_dialog_t *g = g_malloc0(sizeof(dt_gui_presets_edit_dialog_t));
    const char *operation = (const char *)sqlite3_column_text(stmt, 0);
    dt_lib_module_t *lib_module = dt_lib_get_module(operation);

    g->old_id = rowid;
    g->original_name = g_strdup(name_in);
    g->operation = g_strdup(operation);
    g->op_version = sqlite3_column_int(stmt, 1);
    g->module_name = g_strdup(lib_module
                              ? lib_module->name(lib_module)
                              : dt_iop_get_localized_name(operation));
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
    gboolean writeprotect = FALSE;
    name = dt_get_active_preset_name(module, &writeprotect);
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

  dt_gui_presets_show_iop_edit_dialog
    (name, module, G_CALLBACK(_edit_preset_final_callback), NULL, TRUE, TRUE,
     FALSE, GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  g_free(name);
}

static void _menuitem_edit_preset(GSimpleAction *action,
                                  GVariant *parameter,
                                  gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  _edit_preset(NULL, module);
}

static void _menuitem_update_preset(GSimpleAction *action,
                                    GVariant *parameter,
                                    gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  const gchar *name = g_variant_get_string(parameter,  NULL);

  if(!dt_conf_get_bool("plugins/lighttable/preset/ask_before_delete_preset")
     || dt_gui_show_yes_no_dialog(_("update preset?"), "",
                                  _("do you really want to update the preset `%s'?"),
                                  name))
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
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, module->params, module->params_size,
                               SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, module->enabled);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 5, module->blend_params,
                               sizeof(dt_develop_blend_params_t),
                               SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 6, dt_develop_blend_version());
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 7, name, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

static void _menuitem_new_preset(GSimpleAction *action,
                                 GVariant *parameter,
                                 gpointer user_data)
{

  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  // add new preset
  dt_lib_presets_remove(_("new preset"), module->op, module->version());

  // create a shortcut for the new entry
  dt_action_define_preset(&module->so->actions, _("new preset"));

  // then show edit dialog
  _edit_preset(_("new preset"), module);
}

void dt_gui_presets_apply_preset(const gchar* name,
                                 dt_iop_module_t *module)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
     dt_database_get(darktable.db),
     "SELECT op_params, enabled, blendop_params, blendop_version, writeprotect,"
     "       multi_name, multi_name_hand_edited"
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
    const char *multi_name = (const char *)sqlite3_column_text(stmt, 5);
    const int multi_name_hand_edited = sqlite3_column_int(stmt, 6);

    if(op_params && (op_length == module->params_size))
      memcpy(module->params, op_params, op_length);
    else
      memcpy(module->params, module->default_params, module->params_size);

    module->enabled = enabled;

    // if module name has not been hand edited, use preset multi_name
    // or name as module label.

    dt_iop_update_multi_name(module,
                             strlen(multi_name) > 0 ? multi_name : name,
                             multi_name_hand_edited, FALSE, FALSE);

    if(blendop_params
       && (blendop_version == dt_develop_blend_version())
       && (bl_length == sizeof(dt_develop_blend_params_t)))
    {
      dt_iop_commit_blend_params(module, blendop_params, NULL);
    }
    else if(blendop_params
            && dt_develop_blend_legacy_params(module, blendop_params,
                                              blendop_version, module->blend_params,
                                              dt_develop_blend_version(), bl_length) == FALSE)
    {
      // do nothing
    }
    else
    {
      dt_iop_commit_blend_params(module, module->default_blendop_params, NULL);
    }

    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_PRESET_APPLIED, module);

    if(!writeprotect) dt_gui_store_last_preset(name);
  }
  else
    dt_print(DT_DEBUG_ALWAYS,"preset '%s' not found\n",name);

  sqlite3_finalize(stmt);
  dt_iop_gui_update(module);
  // dt_iop_gui_update() runs inside the GUI-update guard, so module GUI
  // callbacks cannot refresh the pixelpipes there.  Apply the preset's
  // parameter changes after leaving that guard; tone equalizer needs this
  // synchronization to rebuild its luminance cache and histogram.
  dt_iop_refresh_all(module);
  dt_dev_add_history_item(darktable.develop, module, FALSE);
  gtk_widget_queue_draw(module->widget);

  if(dt_conf_get_bool("accel/prefer_enabled")
     || dt_conf_get_bool("accel/prefer_unmasked"))
  {
    // rebuild the accelerators
    dt_iop_connect_accels_multi(module->so);
  }
}

void dt_gui_presets_apply_adjacent_preset(dt_iop_module_t *module,
                                          const int direction)
{
  gboolean writeprotect = FALSE;
  gchar *name = dt_get_active_preset_name(module, &writeprotect);
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

  gchar *local_name = dt_util_localize_segmented_name(name, TRUE);

  dt_action_widget_toast(DT_ACTION(module), NULL,
                         _("preset '%s' %s"),
                         name ? local_name : _("no presets"),
                         *extreme ? extreme : "");
  g_free(local_name);
  g_free(name);
}

gboolean dt_gui_presets_autoapply_for_module(dt_iop_module_t *module, GtkWidget *widget)
{
  if(!module || module->actions != DT_ACTION_TYPE_IOP_INSTANCE)
    return FALSE;

  dt_image_t *image = &module->dev->image_storage;

  const gboolean is_display_referred = dt_is_display_referred();
  const gboolean is_scene_referred = dt_is_scene_referred();
  const gboolean has_matrix = dt_image_is_matrix_correction_supported(image);

  char *format_filter = dt_presets_get_filter(image);

  // Take the first auto-applied preset (autoapply = 1) and select first the
  // user's presets if they exist (writeprotect = 0) by ordering on writeprotect.
  // Also make sure we pick the last user's defined preset (ORDER BY rowid DESC).
  char query[2024];
  // clang-format off
  snprintf(query, sizeof(query),
     "SELECT name, op_params, blendop_params"
     " FROM data.presets"
     " WHERE operation = ?1"
     "        AND ((autoapply=1"
     "           AND ((?2 LIKE model AND ?3 LIKE maker) OR (?4 LIKE model AND ?5 LIKE maker))"
     "           AND ?6 LIKE lens AND ?7 BETWEEN iso_min AND iso_max"
     "           AND ?8 BETWEEN exposure_min AND exposure_max"
     "           AND ?9 BETWEEN aperture_min AND aperture_max"
     "           AND ?10 BETWEEN focal_length_min AND focal_length_max"
     "           AND (%s)"
     "           AND operation NOT IN"
     "               ('ioporder', 'metadata', 'export', 'tagging', 'collect', '%s'))"
     "  OR (name = ?13)) AND op_version = ?14"
     " ORDER BY writeprotect ASC, rowid DESC",
     format_filter,
     is_display_referred?"":"basecurve");
  // clang-format on

  g_free(format_filter);

  sqlite3_stmt *stmt;
  const char *workflow_preset = has_matrix && is_display_referred
                                ? BUILTIN_PRESET("display-referred default")
                                : (has_matrix && is_scene_referred
                                   ? BUILTIN_PRESET("scene-referred default")
                                   : "\t\n");

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, image->exif_model, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, image->exif_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, image->camera_alias, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, image->camera_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, image->exif_lens, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, fmaxf(0.0f,
                                              fminf(FLT_MAX, image->exif_iso)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, fmaxf(0.0f,
                                              fminf(1000000, image->exif_exposure)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, fmaxf(0.0f,
                                              fminf(1000000, image->exif_aperture)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, fmaxf(0.0f,
                                               fminf(1000000, image->exif_focal_length)));
  // 0: dontcare, 1: ldr, 2: raw plus monochrome & color
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 13, workflow_preset, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 14, module->version());

  const gboolean found = sqlite3_step(stmt) == SQLITE_ROW;

  if(found)
  {
    if(widget)
    {
      dt_iop_params_t *params = (dt_iop_params_t *)sqlite3_column_blob(stmt, 1);
      dt_develop_blend_params_t *blend_params = (dt_iop_params_t *)sqlite3_column_blob(stmt, 2);
      if(sqlite3_column_bytes(stmt, 1) == module->params_size
         && sqlite3_column_bytes(stmt, 2) == sizeof(dt_develop_blend_params_t))
        dt_bauhaus_update_from_field(module, widget, params, blend_params);
    }
    else
    {
      const char *name = (const char *)sqlite3_column_text(stmt, 0);
      dt_gui_presets_apply_preset(name, module);
    }
  }
  sqlite3_finalize(stmt);

  return found;
}

static guint _click_time = G_MAXUINT;

// need to catch "activate" signal as well to handle keyboard
static void _menuitem_activate_preset(GSimpleAction *action,
                                      GVariant *parameter,
                                      gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  const gchar *preset_name = g_variant_get_string(parameter,  NULL);
  dt_gui_presets_apply_preset(preset_name, module);

  // if(dt_gui_menuitem_activated_by_keyboard(GTK_WIDGET(menuitem)))
  //   dt_gui_presets_apply_preset(g_object_get_data(G_OBJECT(menuitem),
  //                                                 "dt-preset-name"), module);

  // close the menu
  gtk_popover_popdown(GTK_POPOVER(darktable.gui->active_popover_menu));
}

static void _menuitem_activate_quick_preset(GSimpleAction *action,
                                            GVariant *parameter,
                                            gpointer user_data)
{
  GVariant *v_name = g_variant_get_child_value(parameter, 0);
  GVariant *v_ptr  = g_variant_get_child_value(parameter, 1);

  const gchar *preset_name = g_variant_get_string(v_name, NULL);
  guint64 ptr_val          = g_variant_get_uint64(v_ptr);

  g_variant_unref(v_name);
  g_variant_unref(v_ptr);

  g_variant_get(parameter, "(&st)", &preset_name, &ptr_val);
  dt_iop_module_t *module = (dt_iop_module_t *)ptr_val;

  dt_gui_presets_apply_preset(preset_name, module);
}

/* quick presets list
  The list of presets to show is saved in darktablerc
  'plugins/darkroom/quick_preset_list' key
  the content of the key is written in the form :
    ꬹiop_name_0|preset_name_0ꬹꬹiop_name_1|preset_name_1ꬹ...
*/

static gboolean _menuitem_manage_quick_presets_traverse(GtkTreeModel *model,
                                                        GtkTreePath *path,
                                                        GtkTreeIter *iter,
                                                        gpointer data)
{
  gchar **txt = (gchar **)data;
  gchar *preset = NULL;
  gchar *iop_name = NULL;
  gboolean active = FALSE;
  gtk_tree_model_get(model, iter, 1, &active, 3, &iop_name, 4, &preset, -1);

  if(active && preset && iop_name)
  {
    dt_util_str_cat(&*txt, "ꬹ%s|%sꬹ", iop_name, preset);
  }
  g_free(iop_name);
  g_free(preset);

  return FALSE;
}

static void _menuitem_manage_quick_presets_toggle(GtkCellRendererToggle *cell_renderer,
                                                  gchar *path,
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

static void _menuitem_manage_quick_presets(GSimpleAction *action,
                                           GVariant *parameter, 
                                           gpointer user_data)
{
  sqlite3_stmt *stmt;
  GtkWindow *win = GTK_WINDOW(dt_ui_main_window(darktable.gui->ui));
  GtkWidget *dialog = gtk_dialog_new_with_buttons
    (_("manage module layouts"), win,
     GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, NULL, NULL);

  gtk_window_set_default_size(GTK_WINDOW(dialog), DT_PIXEL_APPLY_DPI(400),
                              DT_PIXEL_APPLY_DPI(500));
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_set_name(dialog, "quick-presets-manager");
  gtk_window_set_title(GTK_WINDOW(dialog), _("manage quick presets"));

  GtkTreeViewColumn *col;
  GtkCellRenderer *renderer;
  GtkTreeModel *model;

  GtkWidget *view = gtk_tree_view_new();
  gtk_widget_set_name(view, "quick-presets-manager-list");
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), FALSE);
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection
                              (GTK_TREE_VIEW(view)), GTK_SELECTION_NONE);

  col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);

  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "markup", 0);

  col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);

  renderer = gtk_cell_renderer_toggle_new();
  g_signal_connect(renderer, "toggled",
                   G_CALLBACK(_menuitem_manage_quick_presets_toggle), view);
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "active", 1);
  gtk_tree_view_column_add_attribute(col, renderer, "visible", 2);

  GtkTreeStore *treestore = gtk_tree_store_new(5, G_TYPE_STRING, G_TYPE_BOOLEAN,
                                               G_TYPE_BOOLEAN, G_TYPE_STRING,
                                               G_TYPE_STRING);

  gchar *config = dt_conf_get_string("plugins/darkroom/quick_preset_list");

  GList *m2 = g_list_sort(g_list_copy(darktable.iop),
                          _menuitem_manage_quick_presets_sort);

  for(const GList *modules = m2; modules; modules = g_list_next(modules))
  {
    dt_iop_module_so_t *iop = modules->data;
    GtkTreeIter toplevel;

    /* check if module is visible in current layout */
    if(dt_dev_modulegroups_is_visible(darktable.develop, iop->op))
    {
      // create top entry
      gchar *iopname = g_markup_escape_text(iop->name(), -1);
      gtk_tree_store_insert_with_values(treestore, &toplevel, NULL,
                                        -1, 0, iopname, 1, FALSE, 2, FALSE, -1);
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
        gtk_tree_store_insert_with_values(treestore, NULL, &toplevel, -1,
                                          0, presetname, 1, inlist, 2, TRUE,
                                          3, iop->op, 4, name, -1);
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

  GtkWidget *sw = dt_gui_scroll_wrap(view);
  dt_gui_dialog_add(GTK_DIALOG(dialog), dt_gui_expand(sw));

  gtk_window_set_resizable(GTK_WINDOW(dialog), TRUE);

  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
  gtk_widget_show_all(dialog);
}

void dt_gui_favorite_presets_menu_show(GtkWidget *favorite_presets_button)
{
  GActionGroup *action_group = gtk_widget_get_action_group(favorite_presets_button, "favorites");
  if(action_group == NULL)
  {
    GActionEntry action_entries[] =
    {
      { "activate", _menuitem_activate_quick_preset, "(st)", NULL },
      { "manage",   _menuitem_manage_quick_presets,  NULL,   NULL }
    };

    action_group = G_ACTION_GROUP(g_simple_action_group_new());
    g_action_map_add_action_entries(G_ACTION_MAP(action_group),
                                    action_entries,
                                    G_N_ELEMENTS(action_entries),
                                    NULL);
    gtk_widget_insert_action_group(favorite_presets_button, 
                                   "favorites",
                                   G_ACTION_GROUP(action_group));
  }

  sqlite3_stmt *stmt;
  GMenu *menu = g_menu_new();

  const gboolean default_first =
    dt_conf_get_bool("plugins/darkroom/default_presets_first");

  // clang-format off
  gchar *query = g_strdup_printf("SELECT name, writeprotect"
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

  for(const GList *modules = g_list_last(darktable.develop->iop);
      modules;
      modules = g_list_previous(modules))
  {
    dt_iop_module_t *iop = modules->data;

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
        const gboolean write_protect = (gboolean)sqlite3_column_int(stmt, 1);
        if(retrieve_list)
        {
          // we only show it if module is in favorite
          gchar *key = g_strdup_printf("plugins/darkroom/%s/favorite", iop->so->op);
          const gboolean fav = dt_conf_get_bool(key);
          g_free(key);
          if(fav) dt_util_str_cat(&config, "ꬹ%s|%sꬹ", iop->so->op, name);
        }

        // check that this preset is in the config list
        gchar *txt = g_strdup_printf("ꬹ%s|%sꬹ", iop->so->op, name);
        if(config && strstr(config, txt))
        {
          gchar *local_name =
            write_protect
            ? dt_util_localize_segmented_name(name, TRUE)
            : g_strdup(name);
          gchar *tt = g_markup_printf_escaped("%s %s %s",
                                              iop->name(), iop->multi_name, local_name);
          GMenuItem *mi = g_menu_item_new(tt, NULL);
          g_menu_item_set_action_and_target_value(mi,
                                                  "favorites.activate",
                                                  g_variant_new("(st)", name, (guintptr)iop));
          g_menu_append_item(menu, mi);
          g_object_unref(mi);
          g_free(tt);
          g_free(local_name);
        }
        g_free(txt);
      }

      sqlite3_finalize(stmt);
    }
  }
  if(retrieve_list) dt_conf_set_string("plugins/darkroom/quick_preset_list", config);
  g_free(config);
  g_free(query);

  GMenu *submenu = g_menu_new();
  g_menu_append_section(menu, NULL, G_MENU_MODEL(submenu));
  g_menu_append(submenu, _("manage quick presets list..."), "favorites.manage");

  // popup the menu
  GtkWidget *popover_menu = dt_gui_popover_menu_from_model(GTK_WIDGET(favorite_presets_button), menu);
  gtk_popover_popup(GTK_POPOVER(popover_menu));
}

/* shared preset popup menu: the query walk, writeprotect separators,
 * hierarchy insertion, active highlight and the manage/edit/delete/
 * store/update/prefs tail are identical for the darkroom iop and lib
 * (modulegroups / header) presets menus.  Everything that differs -- the
 * SQL, the row evaluation, the per-item wiring and the trailing prefs
 * section -- is supplied by ops. */
GtkWidget *dt_gui_presets_popup_menu_show(GtkWidget *button,
                                          const dt_gui_presets_menu_ops_t *ops)
{
  GActionGroup *action_group = gtk_widget_get_action_group(button, "presets");
  if(action_group == NULL)
  {
    GActionEntry action_entries[] =
    {
      { "activate", ops->activate_cb, "s",  "''" },
      { "edit",     ops->edit_cb,     NULL, NULL },
      { "delete",   ops->del_cb,      NULL, NULL },
      { "new",      ops->store_cb,    NULL, NULL },
      { "update",   ops->update_cb,   "s",  NULL }
    };

    action_group = G_ACTION_GROUP(g_simple_action_group_new());
    g_action_map_add_action_entries(G_ACTION_MAP(action_group),
                                    action_entries,
                                    G_N_ELEMENTS(action_entries),
                                    ops->data);
    gtk_widget_insert_action_group(button, 
                                   "presets",
                                   G_ACTION_GROUP(action_group));
  }

  GMenu *menu = g_menu_new();

  const gboolean hide_default = dt_conf_get_bool(ops->hide_defaults_pref);

  gchar *query = ops->query(ops->data);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  g_free(query);
  ops->bind(stmt, ops->data);

  int cnt = 0;
  gchar *active_preset_name = NULL;
  gboolean selected_writeprotect = FALSE;
  gboolean found = FALSE;
  int last_wp = -1;
  gchar **prev_split = NULL;
  GMenu *submenu = menu;
  GMenu *mainmenu = submenu;
  GSList *menu_path = NULL; // stack of menuitems which are the parents of submenus on menu_stack
  GAction *item_action;

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
      mainmenu = g_menu_new();
      g_menu_append_section(menu, NULL, G_MENU_MODEL(mainmenu));

      *prev_split[0] = '\0'; // make first level mismatch so we start over
    }

    const char *name = (const char *)sqlite3_column_text(stmt, 0);

    if(darktable.gui->last_preset && strcmp(darktable.gui->last_preset, name) == 0)
      found = TRUE;

    gchar *action = g_strdup_printf("presets.activate::%s", name);
    dt_insert_preset_in_menu_hierarchy(name,
                                       action,
                                       &menu_path,
                                       mainmenu,
                                       &submenu,
                                       &prev_split,
                                       ops->is_default
                                         ? ops->is_default(stmt, ops->data)
                                         : FALSE,
                                       writeprotect);
    g_free(action);

    gboolean wp = FALSE;
    if(ops->is_active(stmt, ops->data, &wp))
    {
      active_preset_name = g_strdup(name);
      selected_writeprotect = wp;
    }

    item_action = g_action_map_lookup_action(G_ACTION_MAP(action_group), "activate");

    if(ops->is_disabled && ops->is_disabled(stmt, ops->data))
    {
      g_simple_action_set_enabled(G_SIMPLE_ACTION(item_action), FALSE);
    }
    else
    {
      g_simple_action_set_enabled(G_SIMPLE_ACTION(item_action), TRUE);
    }

    cnt++;
  }
  sqlite3_finalize(stmt);
  g_slist_free(menu_path);
  g_strfreev(prev_split);

  if(cnt > 0)
  {
    mainmenu = g_menu_new();
    g_menu_append_section(menu, NULL, G_MENU_MODEL(mainmenu));
    cnt = 0;
  }

  // tail: edit+delete / store+update, then the optional prefs section
  if(active_preset_name && !selected_writeprotect)
  {
    g_menu_append(mainmenu, _("edit this preset..."), "presets.edit");
    g_menu_append(mainmenu, _("delete this preset"), "presets.delete");
    cnt++;
  }
  else
  {
    /* no active preset, or the active preset is writeprotect (a shipped
     * default): edit/delete are not allowed, so fall through to store/
     * update instead of leaving a bare separator behind the preset list. */
    g_menu_append(mainmenu, _("store new preset..."), "presets.new");

    if(darktable.gui->last_preset && found)
    {
      char *local_last_name = dt_util_localize_segmented_name(darktable.gui->last_preset,
                                                              TRUE);
      gchar *markup = g_markup_printf_escaped("%s %s",
                                              _("update preset"),
                                              local_last_name);
      
      gchar *action = g_strdup_printf("presets.update::%s", local_last_name);  
      g_menu_append(mainmenu, markup, action);
      g_free(action);

      g_free(local_last_name);
      g_free(markup);
    }
  }

  if(ops->prefs)
  {
    mainmenu = g_menu_new();
    g_menu_append_section(menu, NULL, G_MENU_MODEL(mainmenu));
    ops->prefs(mainmenu, ops->data);
  }

  // mark the active preset
  item_action = g_action_map_lookup_action(G_ACTION_MAP(action_group), "activate");
  g_simple_action_set_state(G_SIMPLE_ACTION(item_action),
                            g_variant_new_string(active_preset_name? active_preset_name : ""));

  g_free(active_preset_name);
  active_preset_name = NULL;

  // popup the menu
  GtkWidget *popover_menu = dt_gui_popover_menu_from_model(button, menu);
  gtk_popover_popup(GTK_POPOVER(popover_menu));

  return popover_menu;
}



/* ---------- darkroom iop preset menu ---------- */

static gchar *_iop_query(gpointer data)
{
  dt_iop_module_t *module = data;
  const dt_image_t *image = &module->dev->image_storage;
  const gboolean default_first = dt_conf_get_bool("modules/default_presets_first");

  if(image)
  {
    char *format_filter = dt_presets_get_filter(image);

    // clang-format off
    gchar *query = g_strdup_printf
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
       "        AND (%s)))"
       " ORDER BY writeprotect %s, LOWER(name), rowid",
       format_filter,
       default_first ? "DESC":"ASC");
    // clang-format on

    g_free(format_filter);
    return query;
  }

  // don't know for which image. show all we got:
  // clang-format off
  return g_strdup_printf("SELECT name, op_params, writeprotect, "
                         "       description, blendop_params, op_version, enabled"
                         " FROM data.presets"
                         " WHERE operation=?1"
                         " ORDER BY writeprotect %s, LOWER(name), rowid",
                         default_first ? "DESC":"ASC");
  // clang-format on
}

static void _iop_bind(sqlite3_stmt *stmt, gpointer data)
{
  dt_iop_module_t *module = data;
  const dt_image_t *image = &module->dev->image_storage;

  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, -1, SQLITE_TRANSIENT);
  if(image)
  {
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, image->exif_model, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, image->exif_maker, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, image->camera_alias, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, image->camera_maker, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, image->exif_lens, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, image->exif_iso);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, image->exif_exposure);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, image->exif_aperture);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, image->exif_focal_length);
  }
}

static gboolean _iop_is_default(sqlite3_stmt *stmt, gpointer data)
{
  dt_iop_module_t *module = data;
  const void *op_params = sqlite3_column_blob(stmt, 1);
  const int32_t op_params_size = sqlite3_column_bytes(stmt, 1);
  const void *blendop_params = sqlite3_column_blob(stmt, 4);
  const int32_t bl_params_size = sqlite3_column_bytes(stmt, 4);

  return module
    && (op_params_size == 0
        || !memcmp(module->default_params, op_params,
                   MIN(op_params_size, module->params_size)))
    && !memcmp(module->default_blendop_params, blendop_params,
               MIN(bl_params_size, sizeof(dt_develop_blend_params_t)));
}

static gboolean _iop_is_disabled(sqlite3_stmt *stmt, gpointer data)
{
  dt_iop_module_t *module = data;
  return sqlite3_column_int(stmt, 5) != module->version();
}

static gboolean _iop_is_active(sqlite3_stmt *stmt, gpointer data, gboolean *writeprotect)
{
  dt_iop_module_t *module = data;
  const void *op_params = sqlite3_column_blob(stmt, 1);
  const int32_t op_params_size = sqlite3_column_bytes(stmt, 1);
  const void *blendop_params = sqlite3_column_blob(stmt, 4);
  const int32_t bl_params_size = sqlite3_column_bytes(stmt, 4);
  const int32_t enabled = sqlite3_column_int(stmt, 6);

  if(((op_params_size == 0
       && !memcmp(module->params, module->default_params,
                  MIN(module->params_size, module->params_size)))
      || (op_params_size > 0
          && !memcmp(module->params, op_params, MIN(op_params_size, module->params_size))))
     && !memcmp(module->blend_params, blendop_params,
                MIN(bl_params_size, sizeof(dt_develop_blend_params_t)))
     && (module->enabled && enabled))
  {
    *writeprotect = sqlite3_column_int(stmt, 2);
    return TRUE;
  }
  return FALSE;
}

static void _iop_prefs(GMenu *menu, gpointer data)
{
  dt_iop_module_t *module = data;

  GActionGroup *action_group = gtk_widget_get_action_group(module->presets_button, "presets");

  // the guide checkbox
  if(module->flags() & IOP_FLAGS_GUIDES_WIDGET)
    dt_guides_add_module_menuitem(menu, action_group, module);

  // the specific parameters
  if(module->set_preferences) module->set_preferences(menu, action_group, module);
}

void dt_gui_presets_popup_menu_show_for_module(GtkWidget *button, dt_iop_module_t *module)
{
  const dt_gui_presets_menu_ops_t ops = {
    .data = module,
    .hide_defaults_pref = "plugins/darkroom/hide_default_presets",
    .query = _iop_query,
    .bind = _iop_bind,
    .is_default = _iop_is_default,
    .is_disabled = _iop_is_disabled,
    .is_active = _iop_is_active,
    .params_size = module->params_size,
    .activate_cb = _menuitem_activate_preset,
    .edit_cb = _menuitem_edit_preset,
    .del_cb = _menuitem_delete_preset,
    .store_cb = _menuitem_new_preset,
    .update_cb = _menuitem_update_preset,
    .prefs = (module->set_preferences || module->flags() & IOP_FLAGS_GUIDES_WIDGET)
               ? _iop_prefs : NULL,
  };

  darktable.gui->active_popover_menu = dt_gui_presets_popup_menu_show(button, &ops);
  _click_time = 0;
}

void dt_gui_presets_update_mml(const char *name,
                               const dt_dev_operation_t op,
                               const int32_t version,
                               const char *maker,
                               const char *model,
                               const char *lens)
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

void dt_gui_presets_update_iso(const char *name,
                               const dt_dev_operation_t op,
                               const int32_t version,
                               const float min,
                               const float max)
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

void dt_gui_presets_update_av(const char *name,
                              const dt_dev_operation_t op,
                              const int32_t version,
                              const float min,
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

void dt_gui_presets_update_tv(const char *name,
                              const dt_dev_operation_t op,
                              const int32_t version,
                              const float min,
                              const float max)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "UPDATE data.presets"
      " SET exposure_min=?1, exposure_max=?2"
      " WHERE operation=?3 AND op_version=?4 AND name=?5",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 1, min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 2, max);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_gui_presets_update_fl(const char *name,
                              const dt_dev_operation_t op,
                              const int32_t version,
                              const float min,
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

void dt_gui_presets_update_format(const char *name,
                                  const dt_dev_operation_t op,
                                  const int32_t version,
                                  const int flag)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE data.presets"
                              " SET format=?1"
                              " WHERE operation=?2 AND op_version=?3 AND name=?4",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, flag);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_gui_presets_update_autoapply(const char *name,
                                     const dt_dev_operation_t op,
                                     const int32_t version,
                                     const gboolean autoapply)
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

void dt_gui_presets_update_filter(const char *name,
                                  const dt_dev_operation_t op,
                                  const int32_t version,
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
