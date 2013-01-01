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
#include "dtgtk/label.h"
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef  struct dt_lib_capture_t
{
  /** Gui part of the module */
  struct
  {
    GtkLabel *label_battery;       // battery label
    GtkLabel *label_battery_value; // battery value label

    GtkLabel *label_jobcode;       // jobcode label
    GtkEntry *entry_jobcode;       // jobcode entry

    GtkButton *button_create;      // create button
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

  dt_conf_set_string("plugins/capture/jobcode", gtk_entry_get_text(lib->gui.entry_jobcode) );
  dt_conf_set_int("plugins/capture/current_filmroll", -1);

  dt_view_tethering_set_job_code(darktable.view_manager, gtk_entry_get_text( lib->gui.entry_jobcode ) );
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
  GtkBox *hbox_battery, *vbox1_battery, *vbox2_battery;
  GtkBox *hbox_jobcode, *vbox1_jobcode, *vbox2_jobcode;

  // Battery
  hbox_battery = GTK_BOX(gtk_hbox_new(FALSE, 5));
  vbox1_battery = GTK_BOX(gtk_vbox_new(TRUE, 5));
  vbox2_battery = GTK_BOX(gtk_vbox_new(TRUE, 5));

  lib->gui.label_battery = GTK_LABEL(gtk_label_new(_("battery")));
  gtk_misc_set_alignment(GTK_MISC(lib->gui.label_battery ), 0.0, 0.5);
  gtk_box_pack_start(vbox1_battery, GTK_WIDGET(lib->gui.label_battery), TRUE, TRUE, 0);

  const char *battery_value=dt_camctl_camera_get_property(darktable.camctl,NULL,"batterylevel");
  lib->gui.label_battery_value = GTK_LABEL(gtk_label_new(battery_value?battery_value:_("n/a")));
  gtk_misc_set_alignment(GTK_MISC(lib->gui.label_battery_value ), 0.0, 0.5);
  gtk_box_pack_start(vbox2_battery, GTK_WIDGET(lib->gui.label_battery_value), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(hbox_battery), GTK_WIDGET(vbox1_battery), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox_battery), GTK_WIDGET(vbox2_battery), TRUE, TRUE, 0);

  // Jobcode
  hbox_jobcode = GTK_BOX(gtk_hbox_new(FALSE, 5));
  vbox1_jobcode = GTK_BOX(gtk_vbox_new(TRUE, 5));
  vbox2_jobcode = GTK_BOX(gtk_vbox_new(TRUE, 5));

  lib->gui.label_jobcode = GTK_LABEL(gtk_label_new(_("jobcode")));
  gtk_misc_set_alignment(GTK_MISC(lib->gui.label_jobcode ), 0.0, 0.5);
  gtk_box_pack_start(vbox1_jobcode, GTK_WIDGET(lib->gui.label_jobcode), TRUE, TRUE, 0);

  lib->gui.entry_jobcode = GTK_ENTRY(gtk_entry_new());
  dt_gui_key_accel_block_on_focus (GTK_WIDGET (lib->gui.entry_jobcode));
  gtk_box_pack_start(vbox2_jobcode, GTK_WIDGET(lib->gui.entry_jobcode), TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(hbox_jobcode), GTK_WIDGET(vbox1_jobcode), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox_jobcode), GTK_WIDGET(vbox2_jobcode), TRUE, TRUE, 0);

  // Create button
  lib->gui.button_create = GTK_BUTTON(gtk_button_new_with_label( _("create") ));

  // Pack widget
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox_battery), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox_jobcode), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(lib->gui.button_create), TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT (lib->gui.button_create), "clicked",
                    G_CALLBACK (create_callback), self);

  gtk_entry_set_text(lib->gui.entry_jobcode, dt_conf_get_string("plugins/capture/jobcode") );
}

void
gui_cleanup (dt_lib_module_t *self)
{
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
