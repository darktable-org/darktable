#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif
#include "common/darktable.h"
#include "gui/gtk.h"
#include <stdlib.h>

// TODO:
// - connect some generic presets to operations
// - connect menu show in imageop.c
// - clean up old menu stuff
// - replace all operation preset combo boxes (except white balance..)
//
// - copy history: treeview list with to-copy-operations, bind menu there as well, custom callback
// - list with custom styles to choose from
//
typedef struct dt_gui_presets_edit_dialog_t
{
  GtkEntry *name;
  GtkTextBuffer *buffer;
  GtkEntry *model, *maker, *lens;
  GtkSpinButton *iso_min, *iso_max;
  GtkSpinButton *exposure_min, *exposure_max;
  GtkSpinButton *aperture_min, *aperture_max;
  GtkSpinButton *focal_length_min, *focal_length_max;
}
dt_gui_presets_edit_dialog_t;

void dt_gui_presets_init()
{
  // create table or fail if it is already there.
  sqlite3_exec(darktable.db, "create table presets "
"(name varchar, description varchar, operation varchar, op_params blob, enabled integer, "
"model varchar, maker varchar, lens varchar, "
"iso_min real, iso_max real, exposure_min real, exposure_max real, aperture_min real, aperture_max real, "
"focal_length_min real, focal_length_max real, "
"writeprotect integer)", NULL, NULL, NULL);
}

void dt_gui_presets_add_generic(const char *name, dt_dev_operation_t op, const void *params, const int32_t params_size, const int32_t enabled)
{
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(darktable.db, "delete from presets where name=?1 and operation=?2", -1, &stmt, NULL);
  sqlite3_bind_text(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, op, strlen(op), SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  sqlite3_prepare_v2(darktable.db, "insert into presets values (?1, '', ?2, ?3, ?4, '', '', '', 0, 0, 0, 0, 0, 0, 0, 0, 1)", -1, &stmt, NULL);
  sqlite3_bind_text(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, op, strlen(op), SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 3, params, params_size, SQLITE_TRANSIENT);
  sqlite3_bind_int (stmt, 4, enabled);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static gchar*
get_preset_name(GtkMenuItem *menuitem)
{
  const gchar *name = gtk_label_get_label(GTK_LABEL(gtk_bin_get_child(GTK_BIN(menuitem))));
  const gchar *c = name;
  if(*c == '<') { while(*c != '>') c++; c++; }
  gchar *pn = g_strdup(c);
  gchar *c2 = pn;
  while(*c2 != '<') c2++; *c2 = '\0';
  c2 = g_strrstr(pn, _("(default)"));
  if(c2 && c2 > pn) *(c2-1) = '\0';
  return pn;
}

static void
menuitem_delete_preset (GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  gchar *name = get_preset_name(menuitem);
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(darktable.db, "delete from presets where name=?1 and operation=?2 and writeprotect=0", -1, &stmt, NULL);
  sqlite3_bind_text(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, module->op, strlen(module->op), SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  g_free(name);
}

static void
edit_preset_response(GtkDialog *dialog, gint response_id, dt_gui_presets_edit_dialog_t *g)
{
  // TODO: commit all the user input fields
  gtk_widget_destroy(GTK_WIDGET(dialog));
  free(g);
}

static void
edit_preset (const char *name_in, dt_iop_module_t *module)
{
  gchar *name = NULL;
  if(name_in == NULL)
  {
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(darktable.db, "select name, op_params, writeprotect from presets where operation=?1", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, module->op, strlen(module->op), SQLITE_TRANSIENT);
    // collect all presets for op from db
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      void *op_params = (void *)sqlite3_column_blob(stmt, 1);
      int32_t op_params_size = sqlite3_column_bytes(stmt, 1);
      if(!memcmp(module->params, op_params, MIN(op_params_size, module->params_size)))
      {
        name = g_strdup((char *)sqlite3_column_text(stmt, 0));
        break;
      }
    }
    sqlite3_finalize(stmt);
    if(name == NULL) return;
  }
  else name = g_strdup(name_in);

  GtkWidget *dialog;
  /* Create the widgets */
  char title[1024];
  GtkWidget *window = glade_xml_get_widget (darktable.gui->main_window, "main_window");
  snprintf(title, 1024, _("edit `%s' for module `%s'"), name, module->name());
  dialog = gtk_dialog_new_with_buttons (title,
      GTK_WINDOW(window),
      GTK_DIALOG_DESTROY_WITH_PARENT,
      GTK_STOCK_OK,
      GTK_RESPONSE_NONE,
      NULL);
  GtkContainer *content_area = GTK_CONTAINER(gtk_dialog_get_content_area (GTK_DIALOG (dialog)));
  GtkBox *box = GTK_BOX(gtk_vbox_new(FALSE, 5));
  // GtkBox *vbox1 = GTK_BOX(gtk_vbox_new(TRUE, 5));
  
  GtkBox *vbox2 = GTK_BOX(gtk_vbox_new(TRUE, 5));
  GtkBox *vbox3 = GTK_BOX(gtk_vbox_new(TRUE, 5));
  GtkBox *vbox4 = GTK_BOX(gtk_vbox_new(TRUE, 5));
  gtk_container_add (content_area, GTK_WIDGET(box));

  dt_gui_presets_edit_dialog_t *g = (dt_gui_presets_edit_dialog_t *)malloc(sizeof(dt_gui_presets_edit_dialog_t));
  g->name = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_text(g->name, name);
  gtk_box_pack_start(box, GTK_WIDGET(g->name), TRUE, TRUE, 0);

  GtkWidget *view = gtk_text_view_new ();
  g->buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
  gtk_box_pack_start(box, view, FALSE, FALSE, 0);

  GtkWidget *label = gtk_label_new(_("automatically apply this preset to images matching"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(box, label, FALSE, FALSE, 0);

  GtkBox *hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));
  gtk_box_pack_start(box,  GTK_WIDGET(hbox),  FALSE, FALSE, 0);
  gtk_box_pack_start(hbox, GTK_WIDGET(vbox2), TRUE, TRUE, 0);
  gtk_box_pack_start(hbox, GTK_WIDGET(vbox3), TRUE, TRUE, 0);
  gtk_box_pack_start(hbox, GTK_WIDGET(vbox4), TRUE, TRUE, 0);

  // model, maker, lens
  g->model = GTK_ENTRY(gtk_entry_new());
  label = gtk_label_new(_("model"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(vbox2, label, FALSE, FALSE, 0);
  gtk_box_pack_start(vbox3, GTK_WIDGET(g->model), FALSE, FALSE, 0);
  gtk_box_pack_start(vbox4, gtk_label_new(""), FALSE, FALSE, 0);
  g->maker = GTK_ENTRY(gtk_entry_new());
  label = gtk_label_new(_("maker"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(vbox2, label, FALSE, FALSE, 0);
  gtk_box_pack_start(vbox3, GTK_WIDGET(g->maker), FALSE, FALSE, 0);
  gtk_box_pack_start(vbox4, gtk_label_new(""), FALSE, FALSE, 0);
  g->lens  = GTK_ENTRY(gtk_entry_new());
  label = gtk_label_new(_("lens"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(vbox2, label, FALSE, FALSE, 0);
  gtk_box_pack_start(vbox3, GTK_WIDGET(g->lens), FALSE, FALSE, 0);
  gtk_box_pack_start(vbox4, gtk_label_new(""), FALSE, FALSE, 0);

  // iso
  label = gtk_label_new(_("iso"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(vbox2, label, FALSE, FALSE, 0);
  g->iso_min = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 25600, 100));
  gtk_spin_button_set_digits(g->iso_min, 0);
  gtk_box_pack_start(vbox3, GTK_WIDGET(g->iso_min), FALSE, FALSE, 0);
  g->iso_max = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 25600, 100));
  gtk_spin_button_set_digits(g->iso_max, 0);
  gtk_box_pack_start(vbox4, GTK_WIDGET(g->iso_max), FALSE, FALSE, 0);

  // exposure
  label = gtk_label_new(_("exposure"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(vbox2, label, FALSE, FALSE, 0);
  g->exposure_min = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0.0001, 60.0, 1.0));
  gtk_spin_button_set_digits(g->exposure_min, 4);
  gtk_box_pack_start(vbox3, GTK_WIDGET(g->exposure_min), FALSE, FALSE, 0);
  g->exposure_max = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0.0001, 60.0, 1.0));
  gtk_spin_button_set_digits(g->exposure_max, 4);
  gtk_box_pack_start(vbox4, GTK_WIDGET(g->exposure_max), FALSE, FALSE, 0);

  // aperture
  label = gtk_label_new(_("aperture"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(vbox2, label, FALSE, FALSE, 0);
  g->aperture_min = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1.0, 30.0, 0.5));
  gtk_spin_button_set_digits(g->aperture_min, 1);
  gtk_box_pack_start(vbox3, GTK_WIDGET(g->aperture_min), FALSE, FALSE, 0);
  g->aperture_max = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1.0, 30.0, 0.5));
  gtk_spin_button_set_digits(g->aperture_max, 1);
  gtk_box_pack_start(vbox4, GTK_WIDGET(g->aperture_max), FALSE, FALSE, 0);

  // focal length
  label = gtk_label_new(_("focal length"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(vbox2, label, FALSE, FALSE, 0);
  g->focal_length_min = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(5, 1000, 10));
  gtk_spin_button_set_digits(g->focal_length_min, 0);
  gtk_box_pack_start(vbox3, GTK_WIDGET(g->focal_length_min), FALSE, FALSE, 0);
  g->focal_length_max = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(5, 1000, 10));
  gtk_spin_button_set_digits(g->focal_length_max, 0);
  gtk_box_pack_start(vbox4, GTK_WIDGET(g->focal_length_max), FALSE, FALSE, 0);

  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(darktable.db, "select description, model, maker, lens, iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max, focal_length_min, focal_length_max from presets where name = ?1 and operation = ?2", -1, &stmt, NULL);
  sqlite3_bind_text(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, module->op, strlen(module->op), SQLITE_TRANSIENT);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    gtk_text_buffer_set_text (g->buffer, (const char *)sqlite3_column_text(stmt, 0), -1);
    gtk_entry_set_text(g->model, (const char *)sqlite3_column_text(stmt, 1));
    gtk_entry_set_text(g->maker, (const char *)sqlite3_column_text(stmt, 2));
    gtk_entry_set_text(g->lens,  (const char *)sqlite3_column_text(stmt, 3));
    gtk_spin_button_set_value(g->iso_min, sqlite3_column_double(stmt, 4));
    gtk_spin_button_set_value(g->iso_max, sqlite3_column_double(stmt, 4));
    gtk_spin_button_set_value(g->exposure_min, sqlite3_column_double(stmt, 5));
    gtk_spin_button_set_value(g->exposure_max, sqlite3_column_double(stmt, 6));
    gtk_spin_button_set_value(g->aperture_min, sqlite3_column_double(stmt, 7));
    gtk_spin_button_set_value(g->aperture_max, sqlite3_column_double(stmt, 8));
    gtk_spin_button_set_value(g->focal_length_min, sqlite3_column_double(stmt, 9));
    gtk_spin_button_set_value(g->focal_length_max, sqlite3_column_double(stmt, 10));
  }
  sqlite3_finalize(stmt);

  g_signal_connect (dialog, "response", G_CALLBACK (edit_preset_response), g);
  gtk_widget_show_all (dialog);
  g_free(name);
}

static void
menuitem_edit_preset (GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  edit_preset (NULL, module);
}

static void
menuitem_new_preset (GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  // add new preset
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(darktable.db, "delete from presets where name=?1 and operation=?2", -1, &stmt, NULL);
  sqlite3_bind_text(stmt, 1, _("new preset"), strlen(_("new preset")), SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, module->op, strlen(module->op), SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  sqlite3_prepare_v2(darktable.db, "insert into presets values (?1, '', ?2, ?3, ?4, '', '', '', 0, 0, 0, 0, 0, 0, 0, 0, 1)", -1, &stmt, NULL);
  sqlite3_bind_text(stmt, 1, _("new preset"), strlen(_("new preset")), SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, module->op, strlen(module->op), SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 3, module->params, module->params_size, SQLITE_TRANSIENT);
  sqlite3_bind_int (stmt, 4, module->enabled);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  // then show edit dialog
  edit_preset (_("new preset"), module);
}

static void
menuitem_pick_preset (GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  gchar *name = get_preset_name(menuitem);
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(darktable.db, "select op_params, enabled from presets where operation = ?1 and name = ?2", -1, &stmt, NULL);
  sqlite3_bind_text(stmt, 1, module->op, strlen(module->op), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, name, strlen(name), SQLITE_TRANSIENT);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *blob = sqlite3_column_blob(stmt, 0);
    int length  = sqlite3_column_bytes(stmt, 0);
    int enabled = sqlite3_column_int(stmt, 1);
    if(blob && length == module->params_size)
    {
      memcpy(module->params, blob, length);
      module->enabled = enabled;
    }
  }
  sqlite3_finalize(stmt);
  g_free(name);
  if(module->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), 1);
  dt_dev_add_history_item(darktable.develop, module);
  gtk_widget_queue_draw(module->widget);
}

static void
menuitem_store_default (GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(darktable.db, "insert or replace into iop_defaults values (?1, ?2, ?3, '%', '%')", -1, &stmt, NULL);
  sqlite3_bind_text(stmt, 1, module->op, strlen(module->op), SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, module->params, module->params_size, SQLITE_TRANSIENT);
  sqlite3_bind_int (stmt, 3, module->enabled);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  dt_iop_load_default_params(module);
}

static void
menuitem_factory_default (GtkMenuItem *menuitem, dt_iop_module_t *module)
{
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(darktable.db, "delete from iop_defaults where operation = ?1", -1, &stmt, NULL);
  sqlite3_bind_text(stmt, 1, module->op, strlen(module->op), SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  dt_iop_load_default_params(module);
}


static void 
dt_gui_presets_popup_menu_show_internal(dt_dev_operation_t op, dt_iop_params_t *params, int32_t params_size, dt_iop_module_t *module, void (*pick_callback)(GtkMenuItem*,void*), void *callback_data)
{
  GtkMenu *menu = darktable.gui->presets_popup_menu;
  if(menu)
    gtk_widget_destroy(GTK_WIDGET(menu));
  darktable.gui->presets_popup_menu = GTK_MENU(gtk_menu_new());
  menu = darktable.gui->presets_popup_menu;

  GtkWidget *mi;
  int active_preset = -1, cnt = 0, writeprotect = 0;
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(darktable.db, "select name, op_params, writeprotect from presets where operation=?1", -1, &stmt, NULL);
  sqlite3_bind_text(stmt, 1, op, strlen(op), SQLITE_TRANSIENT);
  // collect all presets for op from db
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    void *op_params = (void *)sqlite3_column_blob(stmt, 1);
    int32_t op_params_size = sqlite3_column_bytes(stmt, 1);
    int32_t isdefault = 0;
    if(module && !memcmp(module->default_params, op_params, MIN(op_params_size, module->params_size))) isdefault = 1;
    if(!memcmp(params, op_params, MIN(op_params_size, params_size)))
    {
      active_preset = cnt;
      writeprotect = sqlite3_column_int(stmt, 2);
      char *markup;
      mi = gtk_menu_item_new_with_label("");
      if(isdefault)
        markup = g_markup_printf_escaped ("<span weight=\"bold\">%s %s</span>", sqlite3_column_text(stmt, 0), _("(default)"));
      else
        markup = g_markup_printf_escaped ("<span weight=\"bold\">%s</span>", sqlite3_column_text(stmt, 0));
      gtk_label_set_markup (GTK_LABEL (gtk_bin_get_child(GTK_BIN(mi))), markup);
      g_free (markup);
    }
    else
    {
      if(isdefault)
      {
        char *markup;
        mi = gtk_menu_item_new_with_label("");
        markup = g_markup_printf_escaped ("%s %s", sqlite3_column_text(stmt, 0), _("(default)"));
        gtk_label_set_markup (GTK_LABEL (gtk_bin_get_child(GTK_BIN(mi))), markup);
        g_free (markup);
      }
      else mi = gtk_menu_item_new_with_label((const char *)sqlite3_column_text(stmt, 0));
    }
    if(module) g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_pick_preset), module);
    else if(pick_callback) g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(pick_callback), callback_data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    cnt ++;
  }
  sqlite3_finalize(stmt);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

  if(module)
  {
    if(active_preset >= 0)
    {
      if(!writeprotect)
      {
        mi = gtk_menu_item_new_with_label(_("edit this preset.."));
        g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_edit_preset), module);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);

        mi = gtk_menu_item_new_with_label(_("delete this preset"));
        g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_delete_preset), module);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
      }

      mi = gtk_menu_item_new_with_label(_("use preset as default"));
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_store_default), module);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    else
    {
      mi = gtk_menu_item_new_with_label(_("store new preset.."));
      g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_new_preset), module);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }

    mi = gtk_menu_item_new_with_label(_("remove default"));
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_factory_default), module);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
  }
}

void dt_gui_presets_popup_menu_show_for_params(dt_dev_operation_t op, dt_iop_params_t *params, int32_t params_size, void (*pick_callback)(GtkMenuItem*,void*), void *callback_data)
{
  dt_gui_presets_popup_menu_show_internal(op, params, params_size, NULL, pick_callback, callback_data);
}

void dt_gui_presets_popup_menu_show_for_module(dt_iop_module_t *module)
{
  dt_gui_presets_popup_menu_show_internal(module->op, module->params, module->params_size, module, NULL, NULL);
}

