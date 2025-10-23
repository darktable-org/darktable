/*
    This file is part of darktable,
    Copyright (C) 2025 darktable developers.

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
#include "dbsession.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

// override window manager's title bar?
#define USE_HEADER_BAR

typedef struct _dbsession_t {
  GtkWidget *db_screen;
  GtkWidget *entry;
  const char *datadir;
} dt_dbsession_t;

static void _dbsession_screen_destroy(dt_dbsession_t *session)
{
  if(session->db_screen)
    gtk_widget_destroy(session->db_screen);
  session->db_screen = NULL;
}

static void _dbsession_select_db(GtkWidget *button, dt_dbsession_t *session)
{
  const gchar *label = gtk_button_get_label(GTK_BUTTON(button));

  if(strcmp(label, _("default")) == 0)
  {
    dt_conf_set_string("database", "library.db");
    dt_conf_set_string("database/label", "");
  }
  else
  {
    char *dbname = g_strdup_printf("library-%s.db", label);
    dt_conf_set_string("database", dbname);
    dt_conf_set_string("database/label", label);
    g_free(dbname);
  }

  _dbsession_screen_destroy(session);
}

static void _dbsession_new_db(GtkWidget *button, dt_dbsession_t *session)
{
  const gchar *label = gtk_entry_get_text(GTK_ENTRY(session->entry));

  char *dbname = g_strdup_printf("library-%s.db", label);
  dt_conf_set_string("database", dbname);
  dt_conf_set_string("database/label", label);
  g_free(dbname);

  _dbsession_screen_destroy(session);
}

void dt_dbsession_create(const char *datadir)
{
  if(dt_check_gimpmode("file")
     || dt_check_gimpmode("thumb")
     || !dt_conf_get_bool("database/multiple_db"))
  {
    return;
  }

  dt_dbsession_t *session = g_malloc(sizeof(dt_dbsession_t));
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
    gtk_dialog_new_with_buttons(_("darktable - select a database"),
                                NULL, flags,
                                NULL,
                                GTK_RESPONSE_NONE, // <-- fake button list for compiler
                                NULL);

  gtk_window_set_position(GTK_WINDOW(session->db_screen), GTK_WIN_POS_CENTER);

  GList *dbs = dt_read_file_pattern(datadir, "library*.db");

  GtkWidget *l1 = gtk_label_new(_("select an existing database"));
  GtkBox *vbox = GTK_BOX(dt_gui_vbox());
  dt_gui_box_add(vbox, l1);

  const char *current_db =  dt_conf_get_string("database");
  gboolean current_db_found = FALSE;

  for(GList *l = g_list_first(dbs); l; l = g_list_next(l))
  {
    char *name = (char *)l->data;
    GtkWidget *b = NULL;

    if(strcmp(name, "library.db") == 0)
    {
      b = gtk_button_new_with_label(_("default"));
    }
    else if(g_str_has_prefix(name, "library-"))
    {
      // skip "library-" prefix
      char *f = name + strlen("library") + 1;
      // end with the dot
      char *e = f;
      while(*e != '.') e++;
      *e = '\0';

      b = gtk_button_new_with_label(f);
    }

    if(strcmp(name, current_db) == 0)
      current_db_found = TRUE;

    g_signal_connect(G_OBJECT(b), "clicked",
                     G_CALLBACK(_dbsession_select_db), session);
    dt_gui_box_add(vbox, b);
  }
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
  GtkWidget *create = gtk_button_new_with_label(_("create"));
  g_signal_connect(G_OBJECT(create), "clicked",
                   G_CALLBACK(_dbsession_new_db), session);
  dt_gui_box_add(box, session->entry, create);

  GtkBox *content = GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(session->db_screen)));
  dt_gui_box_add(vbox, l2, box);

  dt_gui_box_add(content, vbox);
  gtk_widget_set_name(GTK_WIDGET(vbox), "multiple-db");

  gtk_widget_show_all(session->db_screen);
  while(gtk_dialog_run(GTK_DIALOG(session->db_screen)) == GTK_RESPONSE_ACCEPT);

  _dbsession_screen_destroy(session);
  g_free(session);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
