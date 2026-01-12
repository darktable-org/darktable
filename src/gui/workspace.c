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
  const char *datadir;
} dt_workspace_t;

static void _workspace_screen_destroy(dt_workspace_t *session)
{
  if(session->db_screen)
    gtk_widget_destroy(session->db_screen);
  session->db_screen = NULL;
}

static void _workspace_entry_changed(GtkWidget *button, dt_workspace_t *session)
{
  const gchar *label = gtk_entry_get_text(GTK_ENTRY(session->entry));

  const gboolean status = strlen(label) != 0;

  gtk_widget_set_sensitive(session->create, status);
}

static void _workspace_delete_db(GtkWidget *button, dt_workspace_t *session)
{
  GtkWidget *b = g_object_get_data(G_OBJECT(button), "db");
  const gchar *label = gtk_button_get_label(GTK_BUTTON(b));

  if(dt_gui_show_yes_no_dialog(_("delete workspace"), "wpdialog",
                               _("WARNING\n\ndo you really want to delete the '%s' workspace?"
                                 "\n\nif XMP writing is not activated, the editing work will be lost."),
                               label))
  {
    char FILE[PATH_MAX] = { 0 };

    // db file
    snprintf(FILE, sizeof(FILE),
             "%s/library-%s.db", session->datadir, label);

    if(g_file_test(FILE, G_FILE_TEST_EXISTS))
    {
      GFile *gf = g_file_new_for_path(FILE);
      g_file_delete(gf, NULL, NULL);
      g_object_unref(gf);
    }

    // resource file
    snprintf(FILE, sizeof(FILE),
             "%s/darktablerc-%s", session->datadir, label);

    if(g_file_test(FILE, G_FILE_TEST_EXISTS))
    {
      GFile *gf = g_file_new_for_path(FILE);
      g_file_delete(gf, NULL, NULL);
      g_object_unref(gf);
    }

    // and now, remove/disable the buttons
    gtk_widget_hide(button);
    gtk_widget_hide(b);
  }
}

static void _workspace_select_db(GtkWidget *button, dt_workspace_t *session)
{
  const gchar *label = gtk_button_get_label(GTK_BUTTON(button));

  if(strcmp(label, _("default")) == 0)
  {
    dt_conf_set_string("database", "library.db");
    dt_conf_set_string("workspace/label", "");
  }
  else if(strcmp(label, _("memory")) == 0)
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

  _workspace_screen_destroy(session);
}

static void _workspace_new_db(GtkWidget *button, dt_workspace_t *session)
{
  const gchar *label = gtk_entry_get_text(GTK_ENTRY(session->entry));

  char *dbname = g_strdup_printf("library-%s.db", label);
  dt_conf_set_string("database", dbname);
  dt_conf_set_string("workspace/label", label);
  g_free(dbname);

  _workspace_screen_destroy(session);
}

static GtkBox *_insert_button(dt_workspace_t *session,
                              const char *label,
                              const gboolean with_del)
{
  GtkBox *box = GTK_BOX(dt_gui_hbox());
  GtkWidget *b = gtk_button_new_with_label(label);
  gtk_widget_set_hexpand(GTK_WIDGET(b), TRUE);
  dt_gui_box_add(box, b);
  g_signal_connect(G_OBJECT(b), "clicked",
                   G_CALLBACK(_workspace_select_db), session);

  if(with_del)
  {
    GtkWidget *del = dtgtk_button_new(dtgtk_cairo_paint_remove, 0, NULL);
    g_signal_connect(G_OBJECT(del), "clicked",
                     G_CALLBACK(_workspace_delete_db), session);
    g_object_set_data(G_OBJECT(del), "db", b);
    dt_gui_box_add(box, del);
  }

  dt_gui_dialog_add(session->db_screen, box);
  return box;
}

gboolean dt_workspace_create(const char *datadir)
{
  if(dt_check_gimpmode("file")
     || dt_check_gimpmode("thumb")
     || !dt_conf_get_bool("database/multiple_workspace"))
  {
    return FALSE;
  }

  dt_workspace_t *session = g_malloc(sizeof(dt_workspace_t));
  session->datadir = datadir;

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
                                GTK_RESPONSE_NONE, // <-- fake button list for compiler
                                NULL);

  gtk_window_set_position(GTK_WINDOW(session->db_screen), GTK_WIN_POS_CENTER);

  GList *dbs = dt_read_file_pattern(datadir, "library-*.db");

  GtkWidget *l1 = gtk_label_new(_("select an existing workspace"));
  dt_gui_dialog_add(session->db_screen, l1);

  const char *current_db =  dt_conf_get_string("database");
  gboolean current_db_found = strcmp("default", current_db) == 0 ? TRUE : FALSE;

  // add default workspace
  GtkBox *box = _insert_button(session, _("default"), FALSE);
  // add a memory workspace just after default one
  box = _insert_button(session, _("memory"), FALSE);

  // add now only the non default libraries
  for(GList *l = g_list_first(dbs); l; l = g_list_next(l))
  {
    char *name = (char *)l->data;

    // skip "library-" prefix
    char *f = name + strlen("library") + 1;
    // end with the dot
    char *e = f;
    while(*e != '.') e++;
    *e = '\0';

    box = _insert_button(session, f, TRUE);

    if(strcmp(name, current_db) == 0)
      current_db_found = TRUE;
  }

  g_list_free_full(dbs, g_free);

  //  if the current registerred db is not found reset to
  //  default. This can happens when a DB is renamed or deleted on disk.
  if(!current_db_found)
  {
    dt_conf_set_string("database", "library.db");
  }

  GtkWidget *l2 = gtk_label_new(_("or create a new one"));

  box = GTK_BOX(dt_gui_hbox());
  session->entry = gtk_entry_new();
  g_signal_connect(G_OBJECT(session->entry),
                   "changed", G_CALLBACK(_workspace_entry_changed), session);
  gtk_widget_set_hexpand(session->entry, TRUE);

  session->create = gtk_button_new_with_label(_("create"));
  gtk_widget_set_sensitive(session->create, FALSE);

  g_signal_connect(G_OBJECT(session->create), "clicked",
                   G_CALLBACK(_workspace_new_db), session);
  dt_gui_box_add(box, session->entry, session->create);
  gtk_widget_set_hexpand(GTK_WIDGET(box), TRUE);

  dt_gui_dialog_add(session->db_screen, l2, box);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(session->db_screen));
  gtk_widget_set_name(content, "workspace");

  gtk_widget_show_all(session->db_screen);
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
