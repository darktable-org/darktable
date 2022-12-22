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
#include "common/darktable.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include "gui/gtkentry.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef struct dt_lib_session_t
{
  /** Gui part of the module */
  struct
  {
    GtkLabel *label1;   // Jobcode
    GtkEntry *entry1;   // Jobcode
    GtkButton *button1; // create new
  } gui;

  /** Data part of the module */
  struct
  {

  } data;
} dt_lib_session_t;

const char *name(dt_lib_module_t *self)
{
  return _("session");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"tethering", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}


void gui_reset(dt_lib_module_t *self)
{
}

int position(const dt_lib_module_t *self)
{
  return 999;
}

static void create_callback(GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_session_t *lib = self->data;

  dt_conf_set_string("plugins/session/jobcode", gtk_entry_get_text(lib->gui.entry1));
#ifdef HAVE_GPHOTO2
  dt_view_tethering_set_job_code(darktable.view_manager, gtk_entry_get_text(lib->gui.entry1));
#endif
}

void gui_init(dt_lib_module_t *self)
{
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  self->data = calloc(1, sizeof(dt_lib_session_t));

  // Setup lib data
  dt_lib_session_t *lib = self->data;

  // Setup gui
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkBox *hbox, *vbox1, *vbox2;

  // Session settings
  hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  vbox1 = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  vbox2 = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

  lib->gui.label1 = GTK_LABEL(gtk_label_new(_("jobcode")));
  gtk_widget_set_halign(GTK_WIDGET(lib->gui.label1), GTK_ALIGN_START);
  gtk_box_pack_start(vbox1, GTK_WIDGET(lib->gui.label1), TRUE, TRUE, 0);

  lib->gui.entry1 = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_width_chars(GTK_ENTRY(lib->gui.entry1), 0);
  gtk_box_pack_start(vbox2, GTK_WIDGET(lib->gui.entry1), TRUE, TRUE, 0);

  lib->gui.button1 = GTK_BUTTON(gtk_button_new_with_label(_("create")));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(vbox1), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(vbox2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(lib->gui.button1), TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(lib->gui.button1), "clicked", G_CALLBACK(create_callback), self);

  const char *str = dt_conf_get_string_const("plugins/session/jobcode");
  gtk_entry_set_text(lib->gui.entry1, str);
}

void gui_cleanup(dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
