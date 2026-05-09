/*
    This file is part of darktable,
    Copyright (C) 2025-2026 darktable developers.

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

#include "control/conf.h"
#include "common/utility.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "workspace.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

// override window manager's title bar?
#define USE_HEADER_BAR

typedef struct _workspace_t {
  GtkWidget *db_screen;
  GtkWidget *entry;
  GtkWidget *create;
  GtkWidget *grid;
  GtkWidget *copy_template_check;
  GtkWidget *remember_selection_check;
  GtkWidget *memory_workspace_button;
  GSList *template_radios;
  GtkWidget *template_radio_leader;
  const char *datadir;
  char *selected_template;
} dt_workspace_t;

static gboolean _workspace_label_is_default(const char *label)
{
  return g_ascii_strcasecmp(label, "default") == 0
    || strcmp(label, _("default")) == 0;
}

static gboolean _workspace_label_is_memory(const char *label)
{
  return g_ascii_strcasecmp(label, "memory") == 0
    || strcmp(label, _("memory")) == 0;
}

static gboolean _workspace_label_is_reserved(const char *label)
{
  return _workspace_label_is_default(label)
    || _workspace_label_is_memory(label);
}

static gboolean _workspace_db_file_exists(const char *datadir,
                                          const char *label)
{
  char path[PATH_MAX] = { 0 };
  snprintf(path, sizeof(path), "%s/library-%s.db", datadir, label);
  return g_file_test(path, G_FILE_TEST_EXISTS);
}

static void _workspace_show_message(dt_workspace_t *session,
                                    const char *title,
                                    const char *body)
{
  GtkWidget *const dlg = gtk_message_dialog_new(GTK_WINDOW(session->db_screen),
                                                GTK_DIALOG_DESTROY_WITH_PARENT,
                                                GTK_MESSAGE_WARNING,
                                                GTK_BUTTONS_OK,
                                                "%s", body);
  gtk_window_set_title(GTK_WINDOW(dlg), title);
  GtkWidget *const content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  gtk_widget_set_name(content, "wpdialog");
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dlg);
#endif
  gtk_dialog_run(GTK_DIALOG(dlg));
  gtk_widget_destroy(dlg);
}

static void _workspace_screen_destroy(dt_workspace_t *session)
{
  if(session->db_screen)
    gtk_widget_destroy(session->db_screen);
  session->db_screen = NULL;

  if(session->template_radios)
  {
    g_slist_free(session->template_radios);
    session->template_radios = NULL;
  }
  session->template_radio_leader = NULL;

  if(session->selected_template)
  {
    g_free(session->selected_template);
    session->selected_template = NULL;
  }
}

static void _workspace_copy_settings(const char *source_label,
                                     const char *dest_label,
                                     const char *datadir)
{
  char source_rc[PATH_MAX] = { 0 };
  char dest_rc[PATH_MAX] = { 0 };

  if(_workspace_label_is_default(source_label))
  {
    snprintf(source_rc, sizeof(source_rc), "%s/darktablerc", datadir);
  }
  else
  {
    snprintf(source_rc, sizeof(source_rc), "%s/darktablerc-%s",
             datadir, source_label);
  }

  snprintf(dest_rc, sizeof(dest_rc), "%s/darktablerc-%s",
           datadir, dest_label);

  if(g_file_test(source_rc, G_FILE_TEST_EXISTS))
  {
    GFile *src = g_file_new_for_path(source_rc);
    GFile *dst = g_file_new_for_path(dest_rc);

    g_file_copy(src, dst, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL);

    g_object_unref(src);
    g_object_unref(dst);
  }
}

static gboolean _workspace_should_skip_copied_key(const char *key)
{
  static const char *prefixes[] = {"plugins/lighttable/collect/history",
                                   "plugins/lighttable/collect/string",
                                   "plugins/lighttable/filtering/history",
                                   "ui_last/import_",
                                   NULL};

  static const char *exact[] = {"database",
                                "workspace/label",
                                "plugins/imageio/storage/disk/file_directory",
                                NULL};

  for(int i = 0; exact[i]; i++)
    if(strcmp(key, exact[i]) == 0) return TRUE;

  for(int i = 0; prefixes[i]; i++)
    if(g_str_has_prefix(key, prefixes[i])) return TRUE;

  return FALSE;
}

static void _workspace_sanitize_copied_settings(const char *dest_label,
                                                const char *datadir)
{
  char dest_rc[PATH_MAX] = { 0 };
  snprintf(dest_rc, sizeof(dest_rc), "%s/darktablerc-%s", datadir, dest_label);

  gchar *contents = NULL;
  gsize length = 0;
  if(!g_file_get_contents(dest_rc, &contents, &length, NULL))
    return;

  gchar **lines = g_strsplit(contents, "\n", -1);
  GString *cleaned = g_string_new(NULL);

  for(gint i = 0; lines[i]; i++)
  {
    const char *line = lines[i];
    const char *sep = strchr(line, '=');

    if(!sep)
    {
      g_string_append(cleaned, line);
      g_string_append_c(cleaned, '\n');
      continue;
    }

    char *key = g_strndup(line, sep - line);
    const gboolean skip = _workspace_should_skip_copied_key(key);
    g_free(key);

    if(skip) continue;

    g_string_append(cleaned, line);
    g_string_append_c(cleaned, '\n');
  }

  g_string_append_printf(cleaned, "database=library-%s.db\n", dest_label);
  g_string_append_printf(cleaned, "workspace/label=%s\n", dest_label);

  g_file_set_contents(dest_rc, cleaned->str, cleaned->len, NULL);

  g_string_free(cleaned, TRUE);
  g_strfreev(lines);
  g_free(contents);
}

static void _workspace_template_radio_toggled(GtkToggleButton *radio,
                                              dt_workspace_t *session)
{
  if(!gtk_toggle_button_get_active(radio)) return;

  const gchar *template_name = g_object_get_data(G_OBJECT(radio),
                                                 "template-name");
  if(!template_name) return;

  g_free(session->selected_template);
  session->selected_template = g_strdup(template_name);
}

static void _workspace_selected_template_sync_from_radios(dt_workspace_t *session)
{
  if(!session->template_radios) return;

  for(GSList *l = session->template_radios; l; l = l->next)
  {
    GtkWidget *const radio = GTK_WIDGET(l->data);
    if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(radio))) continue;

    const gchar *const template_name =
      g_object_get_data(G_OBJECT(radio), "template-name");
    if(!template_name) return;

    g_free(session->selected_template);
    session->selected_template = g_strdup(template_name);
    return;
  }
}

static void _workspace_template_radios_set_visible(dt_workspace_t *session,
                                                   const gboolean visible)
{
  for(GSList *l = session->template_radios; l; l = l->next)
  {
    GtkWidget *const radio = GTK_WIDGET(l->data);
    gtk_widget_set_visible(radio, visible);
  }
}

static void _workspace_entry_changed(GtkWidget *entry,
                                     dt_workspace_t *session)
{
  const gchar *label = gtk_entry_get_text(GTK_ENTRY(entry));

  gtk_widget_set_sensitive(session->create, strlen(label) != 0);
  gtk_widget_set_sensitive(session->copy_template_check, strlen(label) != 0);

  if(strlen(label) == 0)
  {
    _workspace_template_radios_set_visible(session, FALSE);
    gtk_toggle_button_set_active
      (GTK_TOGGLE_BUTTON(session->copy_template_check), FALSE);
  }
}

static void _workspace_radio_shell_sync_active(GObject *radio_gobj,
                                               GParamSpec *pspec G_GNUC_UNUSED,
                                               gpointer user_data G_GNUC_UNUSED)
{
  GtkWidget *const radio = GTK_WIDGET(radio_gobj);
  GtkWidget *const shell = g_object_get_data(G_OBJECT(radio), "dt-workspace-shell");
  if(!shell) return;
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(radio)))
    dt_gui_add_class(shell, "dt_workspace_template_radio_on");
  else
    dt_gui_remove_class(shell, "dt_workspace_template_radio_on");
}

static void _workspace_copy_template_toggled(GtkToggleButton *check,
                                             dt_workspace_t *session)
{
  const gboolean copy = gtk_toggle_button_get_active(check);
  _workspace_template_radios_set_visible(session, copy);
  gtk_widget_set_sensitive(session->memory_workspace_button, !copy);
  if(copy)
    _workspace_selected_template_sync_from_radios(session);
}

static void _workspace_delete_db(GtkWidget *button, dt_workspace_t *session)
{
  GtkWidget *b = g_object_get_data(G_OBJECT(button), "db");
  const gchar *label = gtk_button_get_label(GTK_BUTTON(b));

  if(dt_gui_show_yes_no_dialog
     (_("delete workspace"), "wpdialog",
      _("WARNING\n\ndo you really want to delete the '%s' workspace?"
        "\n\nif XMP writing is not activated, the editing work will be lost."),
      label))
  {
    char fpath[PATH_MAX] = { 0 };

    // db file
    snprintf(fpath, sizeof(fpath),
             "%s/library-%s.db", session->datadir, label);

    if(g_file_test(fpath, G_FILE_TEST_EXISTS))
    {
      GFile *gf = g_file_new_for_path(fpath);
      g_file_delete(gf, NULL, NULL);
      g_object_unref(gf);
    }

    // resource file
    snprintf(fpath, sizeof(fpath),
             "%s/darktablerc-%s", session->datadir, label);

    if(g_file_test(fpath, G_FILE_TEST_EXISTS))
    {
      GFile *gf = g_file_new_for_path(fpath);
      g_file_delete(gf, NULL, NULL);
      g_object_unref(gf);
    }

    // and now, remove/disable the buttons
    gtk_widget_hide(button);
    gtk_widget_hide(b);
    {
      GtkWidget *radio = g_object_get_data(G_OBJECT(b), "radio");
      if(radio)
      {
        GtkWidget *const shell = g_object_get_data(G_OBJECT(radio),
                                                   "dt-workspace-shell");

        if(shell && shell != session->grid)
          gtk_widget_hide(shell);
        gtk_widget_hide(radio);
      }
    }

    if(session->selected_template
       && strcmp(session->selected_template, label) == 0)
    {
      g_free(session->selected_template);
      session->selected_template = g_strdup("");
    }

    gtk_widget_queue_resize(session->db_screen);
  }
}

static void _workspace_select_db(GtkWidget *button,
                                 dt_workspace_t *session)
{
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(session->copy_template_check)))
  {
    GtkWidget *const radio = g_object_get_data(G_OBJECT(button), "radio");
    if(radio)
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio), TRUE);
      return;
    }
  }

  const gchar *label = gtk_button_get_label(GTK_BUTTON(button));

  if(_workspace_label_is_default(label))
  {
    dt_conf_set_string("database", "library.db");
    dt_conf_set_string("workspace/label", "");
  }
  else if(_workspace_label_is_memory(label))
  {
    dt_conf_set_string("database", ":memory:");
    dt_conf_set_string("workspace/label", "memory");
  }
  else
  {
    char *dbname = g_strdup_printf("library-%s.db", label);
    dt_conf_set_string("database", dbname);
    dt_conf_set_string("workspace/label", label);
    g_free(dbname);
  }

  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(session->remember_selection_check)))
    dt_conf_set_bool("database/multiple_workspace", FALSE);

  _workspace_screen_destroy(session);
}

static void _workspace_new_db(GtkWidget *button,
                              dt_workspace_t *session)
{
  (void)button;

  gchar *const label = g_strdup(gtk_entry_get_text(GTK_ENTRY(session->entry)));
  g_strstrip(label);

  if(!*label)
  {
    g_free(label);
    return;
  }

  if(_workspace_label_is_reserved(label))
  {
    _workspace_show_message
      (session, _("create workspace"),
       _("WARNING\n\nthe names \"default\" and \"memory\" are reserved."
         "\n\nchoose a different name."));
    g_free(label);
    return;
  }

  if(_workspace_db_file_exists(session->datadir, label))
  {
    gchar *const msg =
      g_strdup_printf(_("WARNING\n\na workspace named \"%s\" already exists."
                        "\n\nchoose a different name."),
                      label);
    _workspace_show_message(session, _("create workspace"), msg);
    g_free(msg);
    g_free(label);
    return;
  }

  char *dbname = g_strdup_printf("library-%s.db", label);
  dt_conf_set_string("database", dbname);
  dt_conf_set_string("workspace/label", label);
  g_free(dbname);

  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(session->copy_template_check)))
  {
    _workspace_selected_template_sync_from_radios(session);
    if(session->selected_template && strlen(session->selected_template) > 0)
    {
      _workspace_copy_settings(session->selected_template, label, session->datadir);
      _workspace_sanitize_copied_settings(label, session->datadir);
    }
  }

  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(session->remember_selection_check)))
    dt_conf_set_bool("database/multiple_workspace", FALSE);

  g_free(label);
  _workspace_screen_destroy(session);
}

static void _workspace_entry_activate(GtkEntry *entry,
                                      dt_workspace_t *session)
{
  (void)entry;
  if(!gtk_widget_get_sensitive(session->create)) return;
  _workspace_new_db(session->create, session);
}

static GtkWidget *_insert_button(dt_workspace_t *session,
                                 const char *label,
                                 const gboolean with_del,
                                 const gboolean with_radio,
                                 const int row)
{
  GtkWidget *radio = NULL;
  GtkWidget *del = NULL;

  if(with_radio)
  {
#if GTK_CHECK_VERSION(4, 0, 0)
    radio = gtk_radio_button_new();
    if(session->template_radio_leader)
      gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(radio),
                                  GTK_TOGGLE_BUTTON(session->template_radio_leader));
    else
      session->template_radio_leader = radio;
#else
    if(session->template_radio_leader)
      radio = gtk_radio_button_new_from_widget
                (GTK_RADIO_BUTTON(session->template_radio_leader));
    else
    {
      radio = gtk_radio_button_new(NULL);
      session->template_radio_leader = radio;
    }
#endif
    session->template_radios = g_slist_prepend(session->template_radios, radio);
    g_object_set_data_full(G_OBJECT(radio), "template-name", g_strdup(label), g_free);
    gtk_widget_set_valign(radio, GTK_ALIGN_CENTER);

    g_object_set_data(G_OBJECT(radio), "dt-workspace-shell", session->grid);

    gtk_grid_attach(GTK_GRID(session->grid), radio, 0, row, 1, 1);

    dt_gui_add_class(radio, "dt_workspace_template_radio");
    g_signal_connect(G_OBJECT(radio), "toggled",
                     G_CALLBACK(_workspace_template_radio_toggled), session);
    g_signal_connect(G_OBJECT(radio), "notify::active",
                     G_CALLBACK(_workspace_radio_shell_sync_active), NULL);
    _workspace_radio_shell_sync_active(G_OBJECT(radio), NULL, NULL);
  }

  GtkWidget *b = gtk_button_new_with_label(label);
  gtk_widget_set_hexpand(GTK_WIDGET(b), TRUE);
  gtk_grid_attach(GTK_GRID(session->grid), b, 1, row, with_del ? 1 : 2, 1);

  g_signal_connect(G_OBJECT(b), "clicked",
                   G_CALLBACK(_workspace_select_db), session);
  if(radio)
    g_object_set_data(G_OBJECT(b), "radio", radio);

  if(with_del)
  {
    del = dtgtk_button_new(dtgtk_cairo_paint_remove, 0, NULL);
    g_signal_connect(G_OBJECT(del), "clicked",
                     G_CALLBACK(_workspace_delete_db), session);
    g_object_set_data(G_OBJECT(del), "db", b);
    gtk_grid_attach(GTK_GRID(session->grid), del, 2, row, 1, 1);

#if GTK_CHECK_VERSION(4, 0, 0)
    gtk_widget_set_hexpand(del, FALSE);
#endif
  }
  return b;
}

static gint _workspace_library_db_compare(gconstpointer a, gconstpointer b)
{
  const char *const sa = (const char *)a;
  const char *const sb = (const char *)b;
  /* same offset as when extracting the workspace label in the loop below */
  const char *const la = sa + strlen("library") + 1;
  const char *const lb = sb + strlen("library") + 1;

  const gint cmp = g_ascii_strcasecmp(la, lb);
  if(cmp != 0) return cmp;

  /* tie-breaker when labels differ only by case (possible on case-sensitive FS) */
  return g_strcmp0(sa, sb);
}

gboolean dt_workspace_create(const char *datadir)
{
  if(dt_check_gimpmode("file")
     || dt_check_gimpmode("thumb")
     || !dt_conf_get_bool("database/multiple_workspace"))
  {
    return FALSE;
  }

  dt_workspace_t *session = g_malloc0(sizeof(dt_workspace_t));
  session->datadir = datadir;
  session->selected_template = g_strdup("");
  session->grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(session->grid), 5);
  gtk_grid_set_row_spacing(GTK_GRID(session->grid), 5);
  gtk_widget_set_valign(session->grid, GTK_ALIGN_CENTER);
  dt_gui_add_class(session->grid, "dt_workspace_template_radio_cell");

  // a simple gtk_dialog_new() leaves us unable to setup the header
  // bar, so use .._with_buttons and just specify a NULbuttonL strings to
  // have no buttons.  We need to pretend to actually have one button,
  // though, to keep the compiler happy
#ifdef USE_HEADER_BAR
  GtkDialogFlags flags = GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_USE_HEADER_BAR;
#else
  GtkDialogFlags flags = GTK_DIALOG_DESTROY_WITH_PARENT;
#endif
  session->db_screen =
    gtk_dialog_new_with_buttons(_("darktable - select a workspace"),
                                NULL, flags,
                                NULL,
                                GTK_RESPONSE_NONE,
                                NULL);

  gtk_window_set_position(GTK_WINDOW(session->db_screen), GTK_WIN_POS_CENTER);

  GList *dbs = dt_read_file_pattern(datadir, "library-*.db");
  if(dbs)
    dbs = g_list_sort(dbs, _workspace_library_db_compare);

  GtkWidget *l1 = gtk_label_new(_("select an existing workspace"));
  dt_gui_dialog_add(session->db_screen, l1);

  const char *current_db = dt_conf_get_string("database");
  gboolean current_db_found = strcmp("default", current_db) == 0 ? TRUE : FALSE;

  // add default workspace
  _insert_button(session, _("default"), FALSE, TRUE, 0);
  // add a memory workspace just after default one
  session->memory_workspace_button = _insert_button(session, _("memory"), FALSE, FALSE, 1);

  int index = 2;

  // add now only the non default libraries
  for(GList *l = g_list_first(dbs); l; l = g_list_next(l), index++)
  {
    char *name = (char *)l->data;

    // skip "library-" prefix
    char *f = name + strlen("library") + 1;
    // end with the dot
    char *e = f;
    while(*e != '.') e++;
    *e = '\0';

    _insert_button(session, f, TRUE, TRUE, index);

    if(strcmp(name, current_db) == 0)
      current_db_found = TRUE;
  }

  dt_gui_dialog_add(session->db_screen, session->grid);

  GtkWidget *remember_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_valign(remember_box, GTK_ALIGN_CENTER);
  session->remember_selection_check =
    gtk_check_button_new_with_label(_("remember selection and don't ask again"));
  gtk_widget_set_valign(session->remember_selection_check, GTK_ALIGN_CENTER);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(session->remember_selection_check), FALSE);
  dt_gui_box_add(remember_box, session->remember_selection_check);
  dt_gui_dialog_add(session->db_screen, remember_box);

  g_list_free_full(dbs, g_free);

  //  if the current registerred db is not found reset to
  //  default. This can happens when a DB is renamed or deleted on disk.
  if(!current_db_found)
  {
    dt_conf_set_string("database", "library.db");
  }

  GtkWidget *l2 = gtk_label_new(_("or create a new one"));

  GtkBox *box = GTK_BOX(dt_gui_hbox());
  session->entry = gtk_entry_new();
  g_signal_connect(G_OBJECT(session->entry),
                   "changed", G_CALLBACK(_workspace_entry_changed), session);
  gtk_widget_set_hexpand(session->entry, TRUE);

  session->create = gtk_button_new_with_label(_("create"));
  gtk_widget_set_sensitive(session->create, FALSE);

  g_signal_connect(G_OBJECT(session->create), "clicked",
                   G_CALLBACK(_workspace_new_db), session);
  g_signal_connect(G_OBJECT(session->entry), "activate",
                   G_CALLBACK(_workspace_entry_activate), session);
  dt_gui_box_add(box, session->entry, session->create);
  gtk_widget_set_hexpand(GTK_WIDGET(box), TRUE);

  dt_gui_dialog_add(session->db_screen, l2, box);

  GtkWidget *copy_check_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_valign(copy_check_box, GTK_ALIGN_CENTER);
  session->copy_template_check =
    gtk_check_button_new_with_label(_("copy settings from existing workspace"));
  gtk_widget_set_valign(session->copy_template_check, GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive(session->copy_template_check, FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(session->copy_template_check), FALSE);
  g_signal_connect(G_OBJECT(session->copy_template_check), "toggled",
                   G_CALLBACK(_workspace_copy_template_toggled), session);
  dt_gui_box_add(copy_check_box, session->copy_template_check);
  dt_gui_dialog_add(session->db_screen, copy_check_box);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(session->db_screen));
  gtk_widget_set_name(content, "workspace");

  gtk_widget_show_all(session->db_screen);
  _workspace_template_radios_set_visible
    (session,
     gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(session->copy_template_check)));
  while(gtk_dialog_run(GTK_DIALOG(session->db_screen)) == GTK_RESPONSE_ACCEPT);

  _workspace_screen_destroy(session);
  g_free(session);

  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
