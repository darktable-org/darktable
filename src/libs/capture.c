/*
    This file is part of darktable,
    copyright (c) 2010-2011 henrik andersson.

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
#include "views/capture.h"
#include "common/darktable.h"
#include "common/camera_control.h"
#include "control/jobs.h"
#include "control/control.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include "gui/gtkentry.h"
#include "dtgtk/label.h"
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef  struct dt_lib_capture_t
{
  /** Gui part of the module */
  struct
  {
    GtkLabel *label1;                           // Jobcode
    GtkEntry *entry1;                         // Jobcode
    GtkLabel *label_namepattern;               // Filename (pattern)
    GtkEntry *entry_namepattern;               // Filename (pattern)
    GtkButton *button1;                     // create new
  } gui;

  /** Data part of the module */
  struct
  {

  } data;
}
dt_lib_capture_t;

const char*
name ()
{
  return _("session");
}

uint32_t views()
{
  return DT_VIEW_TETHERING;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}


void
gui_reset (dt_lib_module_t *self)
{
}

int
position ()
{
  return 999;
}

static void
create_callback(GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self=(dt_lib_module_t *)user_data;
  dt_lib_capture_t *lib=self->data;

  dt_conf_set_string("plugins/capture/jobcode", gtk_entry_get_text(lib->gui.entry1) );
  dt_conf_set_int("plugins/capture/current_filmroll", -1);

  dt_view_tethering_set_job_code(darktable.view_manager, gtk_entry_get_text( lib->gui.entry1 ) );
}

static gboolean
commit_namepattern(GtkEntry *entry, GdkEvent* bob, gpointer user_data)
{
  dt_lib_module_t *self=(dt_lib_module_t *)user_data;
  dt_lib_capture_t *lib=self->data;

  dt_conf_set_string("plugins/capture/storage/namepattern", gtk_entry_get_text(lib->gui.entry_namepattern) );

  dt_view_tethering_set_session_namepattern(darktable.view_manager, gtk_entry_get_text( lib->gui.entry_namepattern ) );

  return FALSE; // FALSE so that the text field also gets the signal
}

void
gui_init (dt_lib_module_t *self)
{
  self->widget = gtk_vbox_new(TRUE, 5);
  self->data = malloc(sizeof(dt_lib_capture_t));
  memset(self->data,0,sizeof(dt_lib_capture_t));

  // Setup lib data
  dt_lib_capture_t *lib=self->data;

  // Setup gui
  self->widget = gtk_vbox_new(FALSE, 5);
  GtkBox *hbox, *vbox1, *vbox2;
  GtkBox *hbox_namepattern, *vbox1_namepattern, *vbox2_namepattern;

  // Session settings
  //gtk_box_pack_start(GTK_BOX(self->widget), dtgtk_label_new("session settings",DARKTABLE_LABEL_TAB|DARKTABLE_LABEL_ALIGN_RIGHT), TRUE, TRUE, 0);
  hbox = GTK_BOX(gtk_hbox_new(FALSE, 5));
  vbox1 = GTK_BOX(gtk_vbox_new(TRUE, 5));
  vbox2 = GTK_BOX(gtk_vbox_new(TRUE, 5));

  hbox_namepattern = GTK_BOX(gtk_hbox_new(FALSE, 5));
  vbox1_namepattern = GTK_BOX(gtk_vbox_new(TRUE, 5));
  vbox2_namepattern = GTK_BOX(gtk_vbox_new(TRUE, 5));

  lib->gui.label1 = GTK_LABEL(gtk_label_new(_("jobcode")));
  gtk_misc_set_alignment(GTK_MISC(lib->gui.label1 ), 0.0, 0.5);
  gtk_box_pack_start(vbox1, GTK_WIDGET(lib->gui.label1), TRUE, TRUE, 0);

  lib->gui.entry1 = GTK_ENTRY(gtk_entry_new());
  dt_gui_key_accel_block_on_focus (GTK_WIDGET (lib->gui.entry1));
  gtk_box_pack_start(vbox2, GTK_WIDGET(lib->gui.entry1), TRUE, TRUE, 0);

  lib->gui.label_namepattern = GTK_LABEL(gtk_label_new(_("filename")));
  gtk_misc_set_alignment(GTK_MISC(lib->gui.label_namepattern ), 0.0, 0.5);
  gtk_box_pack_start(vbox1_namepattern, GTK_WIDGET(lib->gui.label_namepattern), TRUE, TRUE, 0);

  lib->gui.entry_namepattern = GTK_ENTRY(gtk_entry_new());
  dt_gui_key_accel_block_on_focus (GTK_WIDGET (lib->gui.entry_namepattern));
  gtk_box_pack_start(vbox2_namepattern, GTK_WIDGET(lib->gui.entry_namepattern), TRUE, TRUE, 0);

  lib->gui.button1 = GTK_BUTTON(gtk_button_new_with_label( _("create") ));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(vbox1), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(vbox2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(lib->gui.button1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox_namepattern), GTK_WIDGET(vbox1_namepattern), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox_namepattern), GTK_WIDGET(vbox2_namepattern), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox_namepattern), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (lib->gui.button1), "clicked",
                    G_CALLBACK (create_callback), self);

  g_signal_connect (G_OBJECT (lib->gui.entry_namepattern), "focus-out-event",
                    G_CALLBACK (commit_namepattern), self);

  gtk_entry_set_text(lib->gui.entry1, dt_conf_get_string("plugins/capture/jobcode") );
  gtk_entry_set_text(lib->gui.entry_namepattern, dt_conf_get_string("plugins/capture/storage/namepattern") );

  // filename completion
  dt_gtkentry_setup_completion(GTK_ENTRY(lib->gui.entry_namepattern), dt_gtkentry_get_default_path_compl_list());

  // and tooltip
  char *tooltip_text = dt_gtkentry_build_completion_tooltip_text (
                         _("enter the pattern for retrieved images\n⚠\t$(SEQUENCE) is mandatory\nrecognized variables:"),
                         dt_gtkentry_get_default_path_compl_list());
  g_object_set(G_OBJECT(lib->gui.entry_namepattern), "tooltip-text", tooltip_text, (char *)NULL);
  g_free(tooltip_text);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
