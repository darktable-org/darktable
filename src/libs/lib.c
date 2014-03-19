/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2011 Henrik Andersson.

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
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "dtgtk/icon.h"
#include "control/conf.h"
#include "control/control.h"
#include "common/debug.h"
#include <stdlib.h>
#include <stdbool.h>

typedef struct dt_lib_module_info_t
{
  char plugin_name[128];
  int32_t version;
  char params[8192];
  int params_size;
  dt_lib_module_t* module;
}
dt_lib_module_info_t;

typedef struct dt_lib_presets_edit_dialog_t
{
  GtkEntry *name, *description;
  char plugin_name[128];
  int32_t version;
  void *params;
  int32_t params_size;
  gchar *original_name;
  dt_lib_module_t* module;
  gint old_id;
}
dt_lib_presets_edit_dialog_t;

static gchar*
get_preset_name(GtkMenuItem *menuitem)
{
  const gchar *name = gtk_label_get_label(GTK_LABEL(gtk_bin_get_child(GTK_BIN(menuitem))));
  const gchar *c = name;

  // move to marker < if it exists
  while (*c && *c != '<') c++;
  if (!*c) c = name;

  // remove <-> markup tag at beginning.
  if(*c == '<')
  {
    while(*c != '>') c++;
    c++;
  }
  gchar *pn = g_strdup(c);
  gchar *c2 = pn;
  // possibly remove trailing <-> markup tag
  while(*c2 != '<' && *c2 != '\0') c2++;
  if(*c2 == '<') *c2 = '\0';
  c2 = g_strrstr(pn, _("(default)"));
  if(c2 && c2 > pn) *(c2-1) = '\0';
  return pn;
}

static gchar*
get_active_preset_name(dt_lib_module_info_t *minfo)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select name, op_params, writeprotect from presets where operation=?1 and op_version=?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, minfo->plugin_name, strlen(minfo->plugin_name), SQLITE_TRANSIENT);
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

static void
edit_preset_response(GtkDialog *dialog, gint response_id, dt_lib_presets_edit_dialog_t *g)
{
  gint dlg_ret;
  gint is_new = 0;

  if(response_id == GTK_RESPONSE_ACCEPT)
  {
    sqlite3_stmt *stmt;

    // now delete preset, so we can re-insert the new values:
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from presets where name=?1 and operation=?2 and op_version=?3", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, g->original_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, g->plugin_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, g->version);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if ( ((g->old_id >= 0) && (strcmp(g->original_name, gtk_entry_get_text(g->name)) != 0)) || (g->old_id < 0) )
    {

      // editing existing preset with different name or store new preset -> check for a preset with the same name:

      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select name from presets where name = ?1 and operation=?2 and op_version=?3", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, gtk_entry_get_text(g->name), strlen(gtk_entry_get_text(g->name)), SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, g->plugin_name, strlen(g->plugin_name), SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, g->version);

      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        sqlite3_finalize(stmt);

        GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
        GtkWidget *dlg_overwrite = gtk_message_dialog_new (GTK_WINDOW(window),
                                   GTK_DIALOG_DESTROY_WITH_PARENT,
                                   GTK_MESSAGE_WARNING,
                                   GTK_BUTTONS_YES_NO,
                                   _("preset `%s' already exists.\ndo you want to overwrite?"),
                                   gtk_entry_get_text(g->name)
                                                          );
        gtk_window_set_title(GTK_WINDOW (dlg_overwrite), _("overwrite preset?"));
        dlg_ret = gtk_dialog_run (GTK_DIALOG (dlg_overwrite));
        gtk_widget_destroy (dlg_overwrite);

        // if result is BUTTON_NO exit without destroy dialog, to permit other name
        if (dlg_ret == GTK_RESPONSE_NO) return;
      }
      else
      {
        is_new = 1;
        sqlite3_finalize(stmt);
      }

    }

    if (is_new == 0)
    {
      // delete preset, so we can re-insert the new values:
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from presets where name=?1 and operation=?2 and op_version=?3", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, gtk_entry_get_text(g->name), strlen(gtk_entry_get_text(g->name)), SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, g->plugin_name, strlen(g->plugin_name), SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, g->version);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }


    // commit all the user input fields
    char path[1024];
    snprintf(path,sizeof(path),"preset/%s",g->original_name);
    dt_accel_rename_preset_lib(g->module,path,gtk_entry_get_text(g->name));
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into presets (name, description, operation, op_version, op_params, blendop_params, blendop_version, enabled, model, maker, lens, "
                                "iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max, focal_length_min, focal_length_max, writeprotect, "
                                "autoapply, filter, def, format) values (?1, ?2, ?3, ?4, ?5, null, 0, 1, '%', '%', '%', 0, 51200, 0, 100000000, 0, 100000000, 0, 1000, 0, 0, 0, 0, 0)", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, gtk_entry_get_text(g->name), strlen(gtk_entry_get_text(g->name)), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, gtk_entry_get_text(g->description), strlen(gtk_entry_get_text(g->description)), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, g->plugin_name, strlen(g->plugin_name), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, g->version);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 5, g->params, g->params_size, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    dt_gui_store_last_preset (gtk_entry_get_text(g->name));
  }
  gtk_widget_destroy(GTK_WIDGET(dialog));
  g_free(g->original_name);
  free(g);
}

static void
edit_preset (const char *name_in, dt_lib_module_info_t *minfo)
{
  gchar *name = NULL;
  if(name_in == NULL)
  {
    name = get_active_preset_name(minfo);
    if(name == NULL) return;
  }
  else name = g_strdup(name_in);

  GtkWidget *dialog;
  /* Create the widgets */
  char title[1024];
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  snprintf(title, sizeof(title), _("edit `%s'"), name);
  dialog = gtk_dialog_new_with_buttons (title,
                                        GTK_WINDOW(window),
                                        GTK_DIALOG_DESTROY_WITH_PARENT,
                                        GTK_STOCK_OK, GTK_RESPONSE_ACCEPT,
                                        GTK_STOCK_CANCEL, GTK_RESPONSE_REJECT,
                                        NULL);
  GtkContainer *content_area = GTK_CONTAINER(gtk_dialog_get_content_area (GTK_DIALOG (dialog)));
  GtkWidget *alignment = gtk_alignment_new(0.5, 0.5, 1.0, 1.0);
  gtk_alignment_set_padding(GTK_ALIGNMENT(alignment), 5, 5, 5, 5);
  gtk_container_add (content_area, alignment);
  GtkBox *box = GTK_BOX(gtk_vbox_new(FALSE, 5));
  gtk_container_add (GTK_CONTAINER(alignment), GTK_WIDGET(box));

  dt_lib_presets_edit_dialog_t *g = (dt_lib_presets_edit_dialog_t *)g_malloc0(sizeof(dt_lib_presets_edit_dialog_t));
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
  g_object_set(G_OBJECT(g->name), "tooltip-text", _("name of the preset"), (char *)NULL);

  g->description = GTK_ENTRY(gtk_entry_new());
  gtk_box_pack_start(box, GTK_WIDGET(g->description), FALSE, FALSE, 0);
  g_object_set(G_OBJECT(g->description), "tooltip-text", _("description or further information"), (char *)NULL);

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select rowid, description from presets where name = ?1 and operation = ?2 and op_version = ?3", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, minfo->plugin_name, strlen(minfo->plugin_name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, minfo->version);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    g->old_id = sqlite3_column_int(stmt, 0);
    gtk_entry_set_text(g->description, (const char *)sqlite3_column_text(stmt, 1));
  }
  sqlite3_finalize(stmt);

  g_signal_connect (dialog, "response", G_CALLBACK (edit_preset_response), g);
  gtk_widget_show_all (dialog);
}

static void
menuitem_update_preset (GtkMenuItem *menuitem, dt_lib_module_info_t *minfo)
{
  gchar *name = get_preset_name(menuitem);

  // commit all the module fields
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "update presets set operation=?1, op_version=?2, op_params=?3 where name=?4", -1, &stmt, NULL);

  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, minfo->plugin_name, strlen(minfo->plugin_name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, minfo->version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, minfo->params, minfo->params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, strlen(name), SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static void
menuitem_new_preset (GtkMenuItem *menuitem, dt_lib_module_info_t *minfo)
{
  // add new preset
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from presets where name=?1 and operation=?2 and op_version=?3", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, _("new preset"), strlen(_("new preset")), SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, minfo->plugin_name, strlen(minfo->plugin_name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, minfo->version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into presets (name, description, operation, op_version, op_params, blendop_params, blendop_version, enabled, model, maker, lens, "
                              "iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max, focal_length_min, focal_length_max, writeprotect, "
                              "autoapply, filter, def, format) values (?1, '', ?2, ?3, ?4, null, 0, 1, '%', '%', '%', 0, 51200, 0, 100000000, 0, 100000000, 0, 1000, 0, 0, 0, 0, 0)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, _("new preset"), strlen(_("new preset")), SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, minfo->plugin_name, strlen(minfo->plugin_name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, minfo->version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, minfo->params, minfo->params_size, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  // create a shortcut for the new entry
  char path[1024];
  snprintf(path,sizeof(path),"%s/%s",_("preset"), _("new preset"));
  dt_accel_register_lib(minfo->module,path,0,0);
  dt_accel_connect_preset_lib(minfo->module,_("new preset"));
  // then show edit dialog
  edit_preset (_("new preset"), minfo);
}

static void
menuitem_edit_preset (GtkMenuItem *menuitem, dt_lib_module_info_t *minfo)
{
  edit_preset (NULL, minfo);
}

static void
menuitem_delete_preset (GtkMenuItem *menuitem, dt_lib_module_info_t *minfo)
{
  sqlite3_stmt *stmt;
  gchar *name = get_active_preset_name(minfo);
  if(name == NULL) return;
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                      GTK_DIALOG_DESTROY_WITH_PARENT,
                      GTK_MESSAGE_QUESTION,
                      GTK_BUTTONS_YES_NO,
                      _("do you really want to delete the preset `%s'?"), name);
  gtk_window_set_title(GTK_WINDOW (dialog), _("delete preset?"));
  if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES)
  {
    char tmp_path[1024];
    snprintf(tmp_path,sizeof(tmp_path),"%s/%s",_("preset"), name);
    dt_accel_deregister_lib(minfo->module,tmp_path);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from presets where name=?1 and operation=?2 and op_version=?3 and writeprotect=0", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, minfo->plugin_name, strlen(minfo->plugin_name), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, minfo->version);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  g_free(name);
  gtk_widget_destroy (dialog);
}

static void
pick_callback(GtkMenuItem *menuitem, dt_lib_module_info_t *minfo)
{
  // apply preset via set_params
  gchar *pn = get_preset_name(menuitem);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select op_params, writeprotect from presets where operation = ?1 and op_version = ?2 and name = ?3", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, minfo->plugin_name, strlen(minfo->plugin_name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, minfo->version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, pn, strlen(pn), SQLITE_TRANSIENT);

  int res = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *blob = sqlite3_column_blob(stmt, 0);
    int length  = sqlite3_column_bytes(stmt, 0);
    int writeprotect = sqlite3_column_int(stmt, 1);
    if(blob)
    {
      GList *it = darktable.lib->plugins;
      while(it)
      {
        dt_lib_module_t *module = (dt_lib_module_t *)it->data;
        if(!strncmp(module->plugin_name, minfo->plugin_name, 128))
        {
          res = module->set_params(module, blob, length);
          break;
        }
        it = g_list_next(it);
      }
    }

    if (!writeprotect)
      dt_gui_store_last_preset (pn);
  }
  sqlite3_finalize(stmt);
  if(res)
  {
    dt_control_log(_("deleting preset for obsolete module"));
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from presets where operation = ?1 and op_version = ?2 and name = ?3", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, minfo->plugin_name, strlen(minfo->plugin_name), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, minfo->version);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, pn, strlen(pn), SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  g_free(pn);
}

static void
dt_lib_presets_popup_menu_show(dt_lib_module_info_t *minfo)
{
  GtkMenu *menu = darktable.gui->presets_popup_menu;
  if(menu)
    gtk_widget_destroy(GTK_WIDGET(menu));
  darktable.gui->presets_popup_menu = GTK_MENU(gtk_menu_new());
  menu = darktable.gui->presets_popup_menu;

  GtkWidget *mi;
  int active_preset = -1, cnt = 0, writeprotect = 0;
  sqlite3_stmt *stmt;
  // order: get shipped defaults first
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select name, op_params, writeprotect, description from presets where operation=?1 and op_version=?2 order by writeprotect desc, rowid", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, minfo->plugin_name, strlen(minfo->plugin_name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, minfo->version);

  // collect all presets for op from db
  int found = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    void *op_params = (void *)sqlite3_column_blob(stmt, 1);
    int32_t op_params_size = sqlite3_column_bytes(stmt, 1);
    const char *name = (char *)sqlite3_column_text(stmt, 0);

    if (darktable.gui->last_preset
        && strcmp(darktable.gui->last_preset, name)==0)
      found = 1;

    // selected in bold:
    // printf("comparing %d bytes to %d\n", op_params_size, minfo->params_size);
    // for(int k=0;k<op_params_size && !memcmp(minfo->params, op_params, k);k++) printf("compare [%c %c] %d: %d\n",
    // ((const char*)(minfo->params))[k],
    // ((const char*)(op_params))[k],
    // k, memcmp(minfo->params, op_params, k));
    if(op_params_size == minfo->params_size && !memcmp(minfo->params, op_params, op_params_size))
    {
      active_preset = cnt;
      writeprotect = sqlite3_column_int(stmt, 2);
      char *markup;
      mi = gtk_menu_item_new_with_label("");
      markup = g_markup_printf_escaped ("<span weight=\"bold\">%s</span>", name);
      gtk_label_set_markup (GTK_LABEL (gtk_bin_get_child(GTK_BIN(mi))), markup);
      g_free (markup);
    }
    else
    {
      mi = gtk_menu_item_new_with_label((const char *)name);
    }
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(pick_callback), minfo);
    g_object_set(G_OBJECT(mi), "tooltip-text", sqlite3_column_text(stmt, 3), (char *)NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    cnt ++;
  }
  sqlite3_finalize(stmt);

  if(cnt > 0)
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

  // FIXME: this doesn't seem to work.
  if(active_preset >= 0)
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
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_new_preset), minfo);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

    if (darktable.gui->last_preset && found)
    {
      char label[128];
      g_strlcpy(label, _("update preset"), sizeof(label));
      g_strlcat(label, " <span weight=\"bold\">%s</span>", sizeof(label));
      char *markup = g_markup_printf_escaped (label, darktable.gui->last_preset);
      mi = gtk_menu_item_new_with_label("");
      gtk_label_set_markup (GTK_LABEL (gtk_bin_get_child(GTK_BIN(mi))), markup);
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_update_preset), minfo);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
      g_free (markup);
    }
  }
}

static gint
dt_lib_sort_plugins(gconstpointer a, gconstpointer b)
{
  const dt_lib_module_t *am = (const dt_lib_module_t *)a;
  const dt_lib_module_t *bm = (const dt_lib_module_t *)b;
  const int apos = am->position ? am->position() : 0;
  const int bpos = bm->position ? bm->position() : 0;
  return apos - bpos;
}

/* default expandable implementation */
static int _lib_default_expandable()
{
  return 1;
}

static int
dt_lib_load_module (dt_lib_module_t *module, const char *libname, const char *plugin_name)
{
//  char name[1024];
  module->dt = &darktable;
  module->widget = NULL;
  g_strlcpy(module->plugin_name, plugin_name, sizeof(module->plugin_name));
  module->module = g_module_open(libname, G_MODULE_BIND_LAZY);
  if(!module->module) goto error;
  int (*version)();
  if(!g_module_symbol(module->module, "dt_module_dt_version", (gpointer)&(version))) goto error;
  if(version() != dt_version())
  {
    fprintf(stderr, "[lib_load_module] `%s' is compiled for another version of dt (module %d (%s) != dt %d (%s)) !\n", libname, abs(version()), version() < 0 ? "debug" : "opt", abs(dt_version()), dt_version() < 0 ? "debug" : "opt");
    goto error;
  }
  if(!g_module_symbol(module->module, "dt_module_mod_version",  (gpointer)&(module->version)))                goto error;
  if(!g_module_symbol(module->module, "name",                   (gpointer)&(module->name)))                   goto error;
  if(!g_module_symbol(module->module, "views",                  (gpointer)&(module->views)))                  goto error;
  if(!g_module_symbol(module->module, "container",              (gpointer)&(module->container)))              goto error;
  if(!g_module_symbol(module->module, "expandable",             (gpointer)&(module->expandable)))             module->expandable = _lib_default_expandable;
  if(!g_module_symbol(module->module, "init",                   (gpointer)&(module->init)))                   module->init = NULL;

  if(!g_module_symbol(module->module, "gui_reset",              (gpointer)&(module->gui_reset)))              module->gui_reset = NULL;
  if(!g_module_symbol(module->module, "gui_init",               (gpointer)&(module->gui_init)))               goto error;
  if(!g_module_symbol(module->module, "gui_cleanup",            (gpointer)&(module->gui_cleanup)))            goto error;

  if(!g_module_symbol(module->module, "gui_post_expose",        (gpointer)&(module->gui_post_expose)))        module->gui_post_expose = NULL;
  if(!g_module_symbol(module->module, "mouse_leave",            (gpointer)&(module->mouse_leave)))            module->mouse_leave = NULL;
  if(!g_module_symbol(module->module, "mouse_moved",            (gpointer)&(module->mouse_moved)))            module->mouse_moved = NULL;
  if(!g_module_symbol(module->module, "button_released",        (gpointer)&(module->button_released)))        module->button_released = NULL;
  if(!g_module_symbol(module->module, "button_pressed",         (gpointer)&(module->button_pressed)))         module->button_pressed = NULL;
  if(!g_module_symbol(module->module, "configure",              (gpointer)&(module->configure)))              module->configure = NULL;
  if(!g_module_symbol(module->module, "scrolled",               (gpointer)&(module->scrolled)))               module->scrolled = NULL;
  if(!g_module_symbol(module->module, "position",               (gpointer)&(module->position)))               module->position = NULL;
  if((!g_module_symbol(module->module, "get_params",            (gpointer)&(module->get_params))) ||
      (!g_module_symbol(module->module, "set_params",            (gpointer)&(module->set_params))) ||
      (!g_module_symbol(module->module, "init_presets",          (gpointer)&(module->init_presets))))
  {
    // need both at the same time, or none.
    module->set_params   = NULL;
    module->get_params   = NULL;
    module->init_presets = NULL;
  }
  if(!g_module_symbol(module->module, "init_key_accels", (gpointer)&(module->init_key_accels)))        module->init_key_accels = NULL;
  if(!g_module_symbol(module->module, "connect_key_accels", (gpointer)&(module->connect_key_accels)))        module->connect_key_accels = NULL;

  module->accel_closures = NULL;
  module->reset_button = NULL;
  module->presets_button = NULL;

  if (module->gui_reset)
  {
    dt_accel_register_lib(module,
                          NC_("accel", "reset module parameters"), 0, 0);
  }
  if(module->get_params)
  {
    dt_accel_register_lib(module,
                          NC_("accel", "show preset menu"), 0, 0);
  }
#ifdef USE_LUA
  dt_lua_register_lib(darktable.lua_state.state,module);
#endif
  if(module->init) module->init(module);

  return 0;
error:
  fprintf(stderr, "[lib_load_module] failed to open operation `%s': %s\n", plugin_name, g_module_error());
  if(module->module) g_module_close(module->module);
  return 1;
}

static void
init_presets(dt_lib_module_t *module)
{
  if(module->init_presets)
  {
    // only if method exists and no writeprotected (static) preset has been inserted yet.
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select name from presets where operation=?1 and op_version=?2 and writeprotect=1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->name(), -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
    if(sqlite3_step(stmt) != SQLITE_ROW)
    {
      module->init_presets(module);
    }
    sqlite3_finalize(stmt);

  }
}

int
dt_lib_load_modules ()
{
  darktable.lib->plugins = NULL;
  GList *res = NULL;
  dt_lib_module_t *module;
  char plugindir[1024], plugin_name[256];
  const gchar *d_name;
  dt_loc_get_plugindir(plugindir, sizeof(plugindir));
  g_strlcat(plugindir, "/plugins/lighttable", sizeof(plugindir));
  GDir *dir = g_dir_open(plugindir, 0, NULL);
  if(!dir) return 1;
  const int name_offset = strlen(SHARED_MODULE_PREFIX),
            name_end    = strlen(SHARED_MODULE_PREFIX) + strlen(SHARED_MODULE_SUFFIX);
  while((d_name = g_dir_read_name(dir)))
  {
    // get lib*.(so|dll)
    if(!g_str_has_prefix(d_name, SHARED_MODULE_PREFIX)) continue;
    if(!g_str_has_suffix(d_name, SHARED_MODULE_SUFFIX)) continue;
    strncpy(plugin_name, d_name+name_offset, strlen(d_name)-name_end);
    plugin_name[strlen(d_name)-name_end] = '\0';
    module = (dt_lib_module_t *)malloc(sizeof(dt_lib_module_t));
    gchar *libname = g_module_build_path(plugindir, (const gchar *)plugin_name);

    if(dt_lib_load_module(module, libname, plugin_name))
    {
      free(module);
      continue;
    }
    // TODO: init presets
    g_free(libname);
    res = g_list_insert_sorted(res, module, dt_lib_sort_plugins);

    init_presets(module);
    // Calling the keyboard shortcut initialization callback if present
    if(module->init_key_accels)
      module->init_key_accels(module);

  }
  g_dir_close(dir);

  darktable.lib->plugins = res;

  return 0;
}

void
dt_lib_unload_module (dt_lib_module_t *module)
{
  if(module->module) g_module_close(module->module);
}

static void
dt_lib_gui_reset_callback (GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *module = (dt_lib_module_t *)user_data;
  module->gui_reset(module);
}

static void
_preset_popup_posistion(GtkMenu *menu, gint *x,gint *y,gboolean *push_in, gpointer data)
{
  gint w;
  gint ww;
  GtkRequisition requisition;

  w = gdk_window_get_width(gtk_widget_get_window(GTK_WIDGET(data)));
  ww = gdk_window_get_width(gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui)));

  gdk_window_get_origin (gtk_widget_get_window(GTK_WIDGET(data)), x, y);

  gtk_widget_size_request (GTK_WIDGET (menu), &requisition);

  /* align left panel popupmenu to right edge */
  if (*x < ww/2)
    (*x)+=w-requisition.width;

  GtkAllocation allocation_data;
  gtk_widget_get_allocation(GTK_WIDGET(data), &allocation_data);
  (*y)+=allocation_data.height;
}

static void
popup_callback(GtkButton *button, dt_lib_module_t *module)
{
  static dt_lib_module_info_t mi;
  int32_t size = 0;
  g_strlcpy(mi.plugin_name, module->plugin_name, sizeof(mi.plugin_name));
  mi.version = module->version();
  mi.module = module;
  void *params = module->get_params(module, &size);
  if(params)
  {
    g_assert(size <= 4096);
    memcpy(mi.params, params, size);
    mi.params_size = size;
    free(params);
  }
  else mi.params_size = 0;
  dt_lib_presets_popup_menu_show(&mi);
  gtk_menu_popup(darktable.gui->presets_popup_menu, NULL, NULL, _preset_popup_posistion, button, 0, gtk_get_current_event_time());
  gtk_widget_show_all(GTK_WIDGET(darktable.gui->presets_popup_menu));
  gtk_menu_reposition(GTK_MENU(darktable.gui->presets_popup_menu));
}

void dt_lib_gui_set_expanded(dt_lib_module_t *module, gboolean expanded)
{
  if(!module->expander) return;

  /* update expander arrow state */
  GtkWidget *icon;
  GtkWidget *header = gtk_bin_get_child(GTK_BIN(g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(module->expander)),0)));
  gint flags = CPF_DIRECTION_DOWN;
  int c = module->container();

  if ( (c == DT_UI_CONTAINER_PANEL_LEFT_TOP) ||
       (c == DT_UI_CONTAINER_PANEL_LEFT_CENTER) ||
       (c == DT_UI_CONTAINER_PANEL_LEFT_BOTTOM) )
  {
    icon = g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(header)),0);
    if(!expanded)
      flags=CPF_DIRECTION_RIGHT;
  }
  else
  {
    icon = g_list_last(gtk_container_get_children(GTK_CONTAINER(header)))->data;
    if(!expanded)
      flags=CPF_DIRECTION_LEFT;
  }
  dtgtk_icon_set_paint(icon, dtgtk_cairo_paint_solid_arrow, flags);

  /* show / hide plugin widget */
  if(expanded)
  {
    gtk_widget_show_all(module->widget);

    /* register to receive draw events */
    darktable.lib->gui_module = module;

    /* focus the current module */
    for(int k=0; k<DT_UI_CONTAINER_SIZE; k++)
      dt_ui_container_focus_widget(darktable.gui->ui, k, GTK_WIDGET(module->expander));
  }
  else
  {
    gtk_widget_hide(module->widget);

    if(darktable.lib->gui_module == module)
    {
      darktable.lib->gui_module = NULL;
      dt_control_queue_redraw();
    }
  }

  /* store expanded state of module */
  char var[1024];
  snprintf(var, sizeof(var), "plugins/lighttable/%s/expanded", module->plugin_name);
  dt_conf_set_bool(var, gtk_widget_get_visible(module->widget));

}
gboolean dt_lib_gui_get_expanded(dt_lib_module_t *module)
{
  if(!module->expandable()) return true;
  if(!module->expander) return true;
  if(!module->widget) {
    char var[1024];
    snprintf(var, sizeof(var), "plugins/lighttable/%s/expanded", module->plugin_name);
    return dt_conf_get_bool(var);
  }
  return gtk_widget_get_visible(module->widget);
}

static gboolean _lib_plugin_header_button_press(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_lib_module_t *module = (dt_lib_module_t *)user_data;

  if (e->button == 1)
  {
    /* bail out if module is static */
    if(!module->expandable()) return FALSE;

    /* handle shiftclick on expander, hide all except this */
    if (!dt_conf_get_bool("lighttable/ui/single_module") != !(e->state & GDK_SHIFT_MASK))
    {
      GList *it = g_list_first(darktable.lib->plugins);
      uint32_t container = module->container();
      dt_view_t *v = darktable.view_manager->view + darktable.view_manager->current_view;
      gboolean all_other_closed = TRUE;
      while (it)
      {
        dt_lib_module_t *m = (dt_lib_module_t *)it->data;

        if(m != module && container == m->container() && m->expandable() && (m->views() & v->view(v)))
        {
          all_other_closed = all_other_closed && !gtk_widget_get_visible(m->widget);
          dt_lib_gui_set_expanded(m, FALSE);
        }

        it = g_list_next(it);
      }
      if(all_other_closed)
        dt_lib_gui_set_expanded(module, !gtk_widget_get_visible(module->widget));
      else
        dt_lib_gui_set_expanded(module, TRUE);
    }
    else
    {
      /* else just toggle */
      dt_lib_gui_set_expanded(module, !gtk_widget_get_visible(module->widget));
    }

    return TRUE;
  }
  else if (e->button == 2)
  {
    /* show preset popup if any preset for module */

    return TRUE;
  }
  return FALSE;
}

GtkWidget *
dt_lib_gui_get_expander (dt_lib_module_t *module)
{
  /* check if module is expandable */
  if(!module->expandable())
  {
    module->expander = NULL;
    return NULL;
  }

  int bs = 12;

  GtkWidget *expander = gtk_vbox_new(FALSE, 3);
  GtkWidget *header_evb = gtk_event_box_new();
  GtkWidget *header = gtk_hbox_new(FALSE, 0);
  GtkWidget *pluginui_frame = gtk_frame_new(NULL);
  GtkWidget *pluginui = gtk_event_box_new();

  /* setup the header box */
  gtk_container_add(GTK_CONTAINER(header_evb), header);
  g_signal_connect(G_OBJECT(header_evb), "button-press-event", G_CALLBACK(_lib_plugin_header_button_press), module);

  /* setup plugin content frame */
  gtk_frame_set_shadow_type(GTK_FRAME(pluginui_frame),GTK_SHADOW_IN);
  gtk_container_add(GTK_CONTAINER(pluginui_frame),pluginui);

  /* layout the main expander widget */
  gtk_box_pack_start(GTK_BOX(expander), header_evb, TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(expander), pluginui_frame, TRUE, FALSE,0);

  /*
   * initialize the header widgets
   */
  int idx=0;
  GtkWidget *hw[5]= {NULL,NULL,NULL,NULL,NULL};

  /* add the expand indicator icon */
  hw[idx] = dtgtk_icon_new(dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_LEFT);
  gtk_widget_set_size_request(GTK_WIDGET(hw[idx++]),bs,bs);

  /* add module label */
  char label[128];
  g_snprintf(label,sizeof(label),"<span size=\"larger\">%s</span>",module->name());
  hw[idx] = gtk_label_new("");
  gtk_widget_set_name(hw[idx], "panel_label");
  gtk_label_set_markup(GTK_LABEL(hw[idx++]),label);

  /* add reset button if module has implementation */
  if (module->gui_reset)
  {
    hw[idx] = dtgtk_button_new(dtgtk_cairo_paint_reset, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
    module->reset_button = GTK_WIDGET(hw[idx]);
    g_object_set(G_OBJECT(hw[idx]), "tooltip-text", _("reset parameters"), (char *)NULL);
    g_signal_connect (G_OBJECT (hw[idx]), "clicked",
                      G_CALLBACK (dt_lib_gui_reset_callback), module);
  }
  else
    hw[idx] = gtk_fixed_new();
  gtk_widget_set_size_request(GTK_WIDGET(hw[idx++]),bs,bs);

  /* add preset button if module has implementation */
  if (module->get_params)
  {
    hw[idx] = dtgtk_button_new(dtgtk_cairo_paint_presets,CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
    module->presets_button = GTK_WIDGET(hw[idx]);
    g_object_set(G_OBJECT(hw[idx]), "tooltip-text", _("presets"), (char *)NULL);
    g_signal_connect (G_OBJECT (hw[idx]), "clicked", G_CALLBACK (popup_callback), module);
  }
  else
    hw[idx] = gtk_fixed_new();
  gtk_widget_set_size_request(GTK_WIDGET(hw[idx++]),bs,bs);

  /* add a spacer to align buttons with iop buttons (enabled button) */
  hw[idx] = gtk_fixed_new();
  gtk_widget_set_size_request(GTK_WIDGET(hw[idx++]),bs,bs);

  /* lets order header elements depending on left/right side panel placement */
  int c = module->container();
  if ( (c == DT_UI_CONTAINER_PANEL_LEFT_TOP) ||
       (c == DT_UI_CONTAINER_PANEL_LEFT_CENTER) ||
       (c == DT_UI_CONTAINER_PANEL_LEFT_BOTTOM) )
  {
    for(int i=0; i<=4; i++)
      if (hw[i])
        gtk_box_pack_start(GTK_BOX(header), hw[i],i==1?TRUE:FALSE,i==1?TRUE:FALSE,2);
    gtk_misc_set_alignment(GTK_MISC(hw[1]),0.0,0.5);
    dtgtk_icon_set_paint(hw[0], dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_RIGHT);
  }
  else
  {
    for(int i=4; i>=0; i--)
      if (hw[i])
        gtk_box_pack_start(GTK_BOX(header), hw[i],i==1?TRUE:FALSE,i==1?TRUE:FALSE,2);
    gtk_misc_set_alignment(GTK_MISC(hw[1]),1.0,0.5);
    dtgtk_icon_set_paint(hw[0], dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_LEFT);
  }

  /* add module widget into an alignment */
  GtkWidget *al = gtk_alignment_new(1.0, 1.0, 1.0, 1.0);
  gtk_alignment_set_padding(GTK_ALIGNMENT(al), 8, 8, 8, 8);
  gtk_container_add(GTK_CONTAINER(pluginui), al);
  gtk_container_add(GTK_CONTAINER(al), module->widget);
  gtk_widget_show_all(module->widget);
  module->expander = expander;

  return module->expander;
}

void
dt_lib_init (dt_lib_t *lib)
{
  // Setting everything to null initially
  memset(lib, 0, sizeof(dt_lib_t));
  (void)dt_lib_load_modules();
}

void
dt_lib_cleanup (dt_lib_t *lib)
{
  while(lib->plugins)
  {
    dt_lib_module_t *module = (dt_lib_module_t *)(lib->plugins->data);
    dt_lib_unload_module(module);
    free(module);
    lib->plugins = g_list_delete_link(lib->plugins, lib->plugins);
  }
}

void
dt_lib_presets_add(const char *name, const char *plugin_name, const int32_t version, const void *params, const int32_t params_size)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from presets where name=?1 and operation=?2 and op_version=?3", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, plugin_name, strlen(plugin_name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into presets (name, description, operation, op_version, op_params, blendop_params, blendop_version, enabled, model, maker, lens, "
                              "iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max, focal_length_min, focal_length_max, writeprotect, "
                              "autoapply, filter, def, format) values (?1, '', ?2, ?3, ?4, null, 0, 1, '%', '%', '%', 0, 51200, 0, 10000000, 0, 100000000, 0, 1000, 1, 0, 0, 0, 0)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, plugin_name, strlen(plugin_name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, params, params_size, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

gboolean dt_lib_is_visible(dt_lib_module_t *module)
{
  char key[512];
  g_snprintf(key,sizeof(key),"plugins/lighttable/%s/visible", module->plugin_name);
  if (dt_conf_key_exists(key))
    return dt_conf_get_bool(key);

  /* if not key found, always make module visible */
  return TRUE;
}

void dt_lib_set_visible(dt_lib_module_t *module, gboolean visible)
{
  char key[512];
  g_snprintf(key,sizeof(key),"plugins/lighttable/%s/visible", module->plugin_name);
  dt_conf_set_bool(key, visible);
  if(module->widget) {
    if (module->expander){
      gtk_widget_set_visible(GTK_WIDGET(module->expander), visible);
    }else {
      if (visible)
        gtk_widget_show_all(GTK_WIDGET(module->widget));
      else
        gtk_widget_hide(GTK_WIDGET(module->widget));
    }
  }
}

void dt_lib_connect_common_accels(dt_lib_module_t *module)
{
  if(module->reset_button)
    dt_accel_connect_button_lib(module, "reset module parameters",
                                module->reset_button);
  if(module->presets_button)
    dt_accel_connect_button_lib(module, "show preset menu",
                                module->presets_button);
  if(module->init_presets)
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select name from presets where operation=?1 and op_version=?2 order by writeprotect desc, rowid", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->plugin_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      char path[1024];
      snprintf(path,sizeof(path),"%s/%s", _("preset"), (char *)sqlite3_column_text(stmt, 0));
      dt_accel_register_lib(module,path,0,0);
      dt_accel_connect_preset_lib(module,(char *)sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
  }
}

gchar *
dt_lib_get_localized_name(const gchar * plugin_name)
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
        dt_lib_module_t * module = (dt_lib_module_t *)lib->data;
        g_hash_table_insert(module_names, module->plugin_name, _(module->name()));
      }
      while((lib=g_list_next(lib)) != NULL);
    }
  }

  return (gchar*)g_hash_table_lookup(module_names, plugin_name);
}

void dt_lib_colorpicker_set_area(dt_lib_t *lib, float size)
{
  if (!lib->proxy.colorpicker.module || ! lib->proxy.colorpicker.set_sample_area)
    return;
  lib->proxy.colorpicker.set_sample_area(lib->proxy.colorpicker.module, size);
}

void dt_lib_colorpicker_set_point(dt_lib_t *lib, float x, float y)
{
  if (!lib->proxy.colorpicker.module || ! lib->proxy.colorpicker.set_sample_point)
    return;
  lib->proxy.colorpicker.set_sample_point(lib->proxy.colorpicker.module, x, y);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
