/*
    This file is part of darktable,
    copyright (c) 2010 henrik andersson.

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
#include "common/camera_control.h"
#include "control/jobs.h"
#include "control/control.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include "dtgtk/label.h"
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef  struct dt_lib_tethered_t
{
  /** Gui part of the module */
  struct {
    GtkDarktableLabel *header1;         // Session settings
    GtkLabel *label1;                           // Jobcode
    GtkEntry *entry1;                         // Jobcode
  } gui;
  
  /** Data part of the module */
  struct  {
    uint32_t image_id;
    dt_camctl_listener_t *listener;
  } data;
}
dt_lib_tethered_t;

const char*
name ()
{
  return _("tethered shoot");
}

uint32_t views() 
{
  return DT_CAPTURE_VIEW;
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

/** Listener callback from camera control when image are downloaded from camera. */
static void _camera_tethered_downloaded_callback(const dt_camera_t *camera,const char *filename,void *data)
{
  dt_job_t j;
  dt_captured_image_import_job_init(&j,filename);
  dt_control_add_job(darktable.control, &j);
}

void
gui_init (dt_lib_module_t *self)
{
  self->widget = gtk_vbox_new(TRUE, 5);
  self->data = malloc(sizeof(dt_lib_tethered_t));
  memset(self->data,0,sizeof(dt_lib_tethered_t));
  
  // Setup lib data
  dt_lib_tethered_t *lib=self->data;
  lib->data.image_id=-1;
  lib->data.listener = malloc(sizeof(dt_camctl_listener_t));
  memset(lib->data.listener,0,sizeof(dt_camctl_listener_t));
  lib->data.listener->image_downloaded=_camera_tethered_downloaded_callback;
  
  
  // Setup gui
  self->widget = gtk_vbox_new(FALSE, 5);
  GtkBox *hbox, *vbox1, *vbox2;
  
  // Session settings
  gtk_box_pack_start(GTK_BOX(self->widget), dtgtk_label_new("session settings",DARKTABLE_LABEL_TAB|DARKTABLE_LABEL_ALIGN_RIGHT), TRUE, TRUE, 0);
  hbox = GTK_BOX(gtk_hbox_new(FALSE, 5));
  vbox1 = GTK_BOX(gtk_vbox_new(TRUE, 5));
  vbox2 = GTK_BOX(gtk_vbox_new(TRUE, 5));
  
  lib->gui.label1 = GTK_LABEL(gtk_label_new(_("jobcode")));
  gtk_misc_set_alignment(GTK_MISC(lib->gui.label1 ), 0.0, 0.5);
  gtk_box_pack_start(vbox1, GTK_WIDGET(lib->gui.label1), TRUE, TRUE, 0);
  
  lib->gui.entry1 = GTK_ENTRY(gtk_entry_new());
  gtk_box_pack_start(vbox2, GTK_WIDGET(lib->gui.entry1), TRUE, TRUE, 0);
 
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(vbox1), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(vbox2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  
  
  // Initialize session
  
  // create new film roll 
  
  // Register listener and start tethered mode
  dt_camctl_register_listener(darktable.camctl,lib->data.listener);
  dt_camctl_tether_mode(darktable.camctl,NULL,TRUE);
  
}

void
gui_cleanup (dt_lib_module_t *self)
{
}

