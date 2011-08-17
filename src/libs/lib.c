/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "control/conf.h"
#include "control/control.h"
#include "common/debug.h"
#include <stdlib.h>

typedef struct dt_lib_module_info_t
{
  char plugin_name[128];
  char params[8192];
  int params_size;
}
dt_lib_module_info_t;

typedef struct dt_lib_presets_edit_dialog_t
{
  GtkEntry *name, *description;
  char plugin_name[128];
  void *params;
  int32_t params_size;
}
dt_lib_presets_edit_dialog_t;

static gchar*
get_preset_name(GtkMenuItem *menuitem)
{
  const gchar *name = gtk_label_get_label(GTK_LABEL(gtk_bin_get_child(GTK_BIN(menuitem))));
  const gchar *c = name;
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
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select name, op_params, writeprotect from presets where operation=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, minfo->plugin_name, strlen(minfo->plugin_name), SQLITE_TRANSIENT);
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
  // commit all the user input fields
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "insert into presets (name, description, operation, op_params, blendop_params, enabled, model, maker, lens, "
             "iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max, focal_length_min, focal_length_max, writeprotect, "
             "autoapply, filter, def, isldr) values (?1, ?2, ?3, ?4, null, 1, '%', '%', '%', 0, 51200, 0, 100000000, 0, 100000000, 0, 1000, 0, 0, 0, 0, 0)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, gtk_entry_get_text(g->name), strlen(gtk_entry_get_text(g->name)), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, gtk_entry_get_text(g->description), strlen(gtk_entry_get_text(g->description)), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, g->plugin_name, strlen(g->plugin_name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, g->params, g->params_size, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  gtk_widget_destroy(GTK_WIDGET(dialog));
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
  GtkWidget *window = darktable.gui->widgets.main_window;
  snprintf(title, 1024, _("edit `%s'"), name);
  dialog = gtk_dialog_new_with_buttons (title,
                                        GTK_WINDOW(window),
                                        GTK_DIALOG_DESTROY_WITH_PARENT,
                                        GTK_STOCK_OK,
                                        GTK_RESPONSE_NONE,
                                        NULL);
  GtkContainer *content_area = GTK_CONTAINER(gtk_dialog_get_content_area (GTK_DIALOG (dialog)));
  GtkWidget *alignment = gtk_alignment_new(0.5, 0.5, 1.0, 1.0);
  gtk_alignment_set_padding(GTK_ALIGNMENT(alignment), 5, 5, 5, 5);
  gtk_container_add (content_area, alignment);
  GtkBox *box = GTK_BOX(gtk_vbox_new(FALSE, 5));
  gtk_container_add (GTK_CONTAINER(alignment), GTK_WIDGET(box));

  dt_lib_presets_edit_dialog_t *g = (dt_lib_presets_edit_dialog_t *)g_malloc0(sizeof(dt_lib_presets_edit_dialog_t));
  g_strlcpy(g->plugin_name, minfo->plugin_name, 128);
  g->params_size = minfo->params_size;
  g->params = minfo->params;
  g->name = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_text(g->name, name);
  gtk_box_pack_start(box, GTK_WIDGET(g->name), FALSE, FALSE, 0);
  g_object_set(G_OBJECT(g->name), "tooltip-text", _("name of the preset"), (char *)NULL);

  g->description = GTK_ENTRY(gtk_entry_new());
  gtk_box_pack_start(box, GTK_WIDGET(g->description), FALSE, FALSE, 0);
  g_object_set(G_OBJECT(g->description), "tooltip-text", _("description or further information"), (char *)NULL);

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select description from presets where name = ?1 and operation = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, minfo->plugin_name, strlen(minfo->plugin_name), SQLITE_TRANSIENT);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    gtk_entry_set_text(g->description, (const char *)sqlite3_column_text(stmt, 0));
  }
  sqlite3_finalize(stmt);

  // now delete preset, so we can re-insert the new values:
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "delete from presets where name=?1 and operation=?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, minfo->plugin_name, strlen(minfo->plugin_name), SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  g_signal_connect (dialog, "response", G_CALLBACK (edit_preset_response), g);
  gtk_widget_show_all (dialog);
  g_free(name);
}

static void
menuitem_new_preset (GtkMenuItem *menuitem, dt_lib_module_info_t *minfo)
{
  // add new preset
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "delete from presets where name=?1 and operation=?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, _("new preset"), strlen(_("new preset")), SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, minfo->plugin_name, strlen(minfo->plugin_name), SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "insert into presets (name, description, operation, op_params, blendop_params, enabled, model, maker, lens, "
             "iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max, focal_length_min, focal_length_max, writeprotect, "
             "autoapply, filter, def, isldr) values (?1, '', ?2, ?3, null, 1, '%', '%', '%', 0, 51200, 0, 100000000, 0, 100000000, 0, 1000, 0, 0, 0, 0, 0)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, _("new preset"), strlen(_("new preset")), SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, minfo->plugin_name, strlen(minfo->plugin_name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, minfo->params, minfo->params_size, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
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
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "delete from presets where name=?1 and operation=?2 and writeprotect=0", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, minfo->plugin_name, strlen(minfo->plugin_name), SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  g_free(name);
}

static void
pick_callback(GtkMenuItem *menuitem, dt_lib_module_info_t *minfo)
{
  // apply preset via set_params
  gchar *pn = get_preset_name(menuitem);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select op_params from presets where operation = ?1 and name = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, minfo->plugin_name, strlen(minfo->plugin_name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, pn, strlen(pn), SQLITE_TRANSIENT);

  int res = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *blob = sqlite3_column_blob(stmt, 0);
    int length  = sqlite3_column_bytes(stmt, 0);
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
  }
  sqlite3_finalize(stmt);
  if(res)
  {
    dt_control_log(_("deleting preset for obsolete module"));
    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "delete from presets where operation = ?1 and name = ?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, minfo->plugin_name, strlen(minfo->plugin_name), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, pn, strlen(pn), SQLITE_TRANSIENT);
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
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select name, op_params, writeprotect, description from presets where operation=?1 order by writeprotect desc, rowid", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, minfo->plugin_name, strlen(minfo->plugin_name), SQLITE_TRANSIENT);

  // collect all presets for op from db
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    void *op_params = (void *)sqlite3_column_blob(stmt, 1);
    int32_t op_params_size = sqlite3_column_bytes(stmt, 1);
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
      markup = g_markup_printf_escaped ("<span weight=\"bold\">%s</span>", sqlite3_column_text(stmt, 0));
      gtk_label_set_markup (GTK_LABEL (gtk_bin_get_child(GTK_BIN(mi))), markup);
      g_free (markup);
    }
    else
    {
      mi = gtk_menu_item_new_with_label((const char *)sqlite3_column_text(stmt, 0));
    }
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(pick_callback), minfo);
    g_object_set(G_OBJECT(mi), "tooltip-text", sqlite3_column_text(stmt, 3), (char *)NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    cnt ++;
  }
  sqlite3_finalize(stmt);
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

static int
dt_lib_load_module (dt_lib_module_t *module, const char *libname, const char *plugin_name)
{
//  char name[1024];
  module->dt = &darktable;
  module->widget = NULL;
  g_strlcpy(module->plugin_name, plugin_name, 20);
  module->module = g_module_open(libname, G_MODULE_BIND_LAZY);
  if(!module->module) goto error;
  int (*version)();
  if(!g_module_symbol(module->module, "dt_module_dt_version", (gpointer)&(version))) goto error;
  if(version() != dt_version())
  {
    fprintf(stderr, "[lib_load_module] `%s' is compiled for another version of dt (module %d (%s) != dt %d (%s)) !\n", libname, abs(version()), version() < 0 ? "debug" : "opt", abs(dt_version()), dt_version() < 0 ? "debug" : "opt");
    goto error;
  }
  if(!g_module_symbol(module->module, "name",                   (gpointer)&(module->name)))                   goto error;
  if(!g_module_symbol(module->module, "views",                  (gpointer)&(module->views)))                  goto error;
  if(!g_module_symbol(module->module, "gui_reset",              (gpointer)&(module->gui_reset)))              goto error;
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

//  if (module->gui_reset)
//  {
//    snprintf(name, 1024, "<Darktable>/lighttable/plugins/%s/reset plugin parameters",module->plugin_name);
//    dtgtk_button_init_accel(darktable.control->accels_lighttable,name);
//  }
//  if(module->get_params)
//  {
//    snprintf(name, 1024, "<Darktable>/lighttable/plugins/%s/show preset menu",module->plugin_name);
//    dtgtk_button_init_accel(darktable.control->accels_lighttable,name);
//  }

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
    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select * from presets where operation=?1 and writeprotect=1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->name(), -1, SQLITE_TRANSIENT);
    if(sqlite3_step(stmt) != SQLITE_ROW) module->init_presets(module);
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
  dt_util_get_plugindir(plugindir, 1024);
  g_strlcat(plugindir, "/plugins/lighttable", 1024);
  GDir *dir = g_dir_open(plugindir, 0, NULL);
  if(!dir) return 1;
  while((d_name = g_dir_read_name(dir)))
  {
    // get lib*.so
    if(strncmp(d_name, "lib", 3)) continue;
    if(strncmp(d_name + strlen(d_name) - 3, ".so", 3)) continue;
    strncpy(plugin_name, d_name+3, strlen(d_name)-6);
    plugin_name[strlen(d_name)-6] = '\0';
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

//     module->factory_params = malloc(module->params_size);
//     memcpy(module->factory_params, module->default_params, module->params_size);
//     module->factory_enabled = module->default_enabled;
    init_presets(module);
    // Calling the keyboard shortcut initialization callback if present
    if(module->init_key_accels)
      (module->init_key_accels)(module);
//     dt_iop_load_default_params(module);

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
dt_lib_gui_expander_callback (GObject *object, GParamSpec *param_spec, gpointer user_data)
{
  GtkExpander *expander = GTK_EXPANDER (object);
  dt_lib_module_t *module = (dt_lib_module_t *)user_data;

  char var[1024];
  snprintf(var, 1024, "plugins/lighttable/%s/expanded", module->plugin_name);
  dt_conf_set_bool(var, gtk_expander_get_expanded (expander));

  if (gtk_expander_get_expanded (expander))
  {
    gtk_widget_show_all(module->widget);
    // register to receive draw events
    darktable.lib->gui_module = module;
    GtkContainer *box = GTK_CONTAINER(darktable.gui->widgets.plugins_vbox);
    gtk_container_set_focus_child(box, GTK_WIDGET(module->expander));
    // redraw gui (in case post expose is set)
    gtk_widget_queue_resize(darktable.gui->widgets.plugins_vbox);
    dt_control_gui_queue_draw();
  }
  else
  {
    if(darktable.lib->gui_module == module)
    {
      darktable.lib->gui_module = NULL;
      dt_control_gui_queue_draw();
    }
    gtk_widget_hide_all(module->widget);
  }
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
  gint w,h;
  GtkRequisition requisition;
  gdk_window_get_size(GTK_WIDGET(data)->window,&w,&h);
  gdk_window_get_origin (GTK_WIDGET(data)->window, x, y);
  gtk_widget_size_request (GTK_WIDGET (menu), &requisition);
  (*x)+=w-requisition.width;
  (*y)+=GTK_WIDGET(data)->allocation.height;
}

static void
popup_callback(GtkButton *button, dt_lib_module_t *module)
{
  static dt_lib_module_info_t mi;
  int32_t size = 0;
  g_strlcpy(mi.plugin_name, module->plugin_name, 128);
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

GtkWidget *
dt_lib_gui_get_expander (dt_lib_module_t *module)
{
//  char name[1024];
  GtkHBox *hbox = GTK_HBOX(gtk_hbox_new(FALSE, 0));
  GtkVBox *vbox = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  module->expander = GTK_EXPANDER(gtk_expander_new((const gchar *)(module->name())));

  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(module->expander), TRUE, TRUE, 0);
  GtkDarktableButton *resetbutton = DTGTK_BUTTON(dtgtk_button_new(dtgtk_cairo_paint_reset, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER));
  gtk_widget_set_size_request(GTK_WIDGET(resetbutton),13,13);
  g_object_set(G_OBJECT(resetbutton), "tooltip-text", _("reset parameters"), (char *)NULL);
//  snprintf(name, 1024, "<Darktable>/lighttable/plugins/%s/reset plugin parameters",module->plugin_name);
//  dtgtk_button_set_accel(resetbutton,darktable.control->accels_lighttable,name);
  gtk_box_pack_end  (GTK_BOX(hbox), GTK_WIDGET(resetbutton), FALSE, FALSE, 0);
  if(module->get_params)
  {
    GtkDarktableButton *presetsbutton = DTGTK_BUTTON(dtgtk_button_new(dtgtk_cairo_paint_presets,CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER));
    gtk_widget_set_size_request(GTK_WIDGET(presetsbutton),13,13);
    g_object_set(G_OBJECT(presetsbutton), "tooltip-text", _("presets"), (char *)NULL);
//    snprintf(name, 1024, "<Darktable>/lighttable/plugins/%s/show preset menu",module->plugin_name);
//    dtgtk_button_set_accel(presetsbutton,darktable.control->accels_lighttable,name);
    gtk_box_pack_end  (GTK_BOX(hbox), GTK_WIDGET(presetsbutton), FALSE, FALSE, 0);
    g_signal_connect (G_OBJECT (presetsbutton), "clicked", G_CALLBACK (popup_callback), module);
  }
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  GtkWidget *al = gtk_alignment_new(1.0, 1.0, 1.0, 1.0);
  gtk_alignment_set_padding(GTK_ALIGNMENT(al), 10, 10, 10, 5);
  gtk_box_pack_start(GTK_BOX(vbox), al, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(al), module->widget);

  g_signal_connect (G_OBJECT (resetbutton), "clicked",
                    G_CALLBACK (dt_lib_gui_reset_callback), module);
  g_signal_connect (G_OBJECT (module->expander), "notify::expanded",
                    G_CALLBACK (dt_lib_gui_expander_callback), module);
  gtk_expander_set_spacing(module->expander, 10);
  gtk_widget_hide_all(module->widget);
  gtk_expander_set_expanded(module->expander, FALSE);
  GtkWidget *evb = gtk_event_box_new();

  gtk_container_set_border_width(GTK_CONTAINER(evb), 0);
  gtk_container_add(GTK_CONTAINER(evb), GTK_WIDGET(vbox));

  return evb;
}

void
dt_lib_init (dt_lib_t *lib)
{
  lib->gui_module = NULL;
  lib->plugins = NULL;
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
dt_lib_presets_add(const char *name, const char *plugin_name, const void *params, const int32_t params_size)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "delete from presets where name=?1 and operation=?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, plugin_name, strlen(plugin_name), SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "insert into presets (name, description, operation, op_params, blendop_params, enabled, model, maker, lens, "
             "iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max, focal_length_min, focal_length_max, writeprotect, "
             "autoapply, filter, def, isldr) values (?1, '', ?2, ?3, null, 1, '%', '%', '%', 0, 51200, 0, 10000000, 0, 100000000, 0, 1000, 1, 0, 0, 0, 0)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, plugin_name, strlen(plugin_name), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, params, params_size, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
