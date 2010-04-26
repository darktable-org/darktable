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

typedef struct dt_lib_camera_property_t
{
  /** label of property */
  GtkLabel *label;
  /** the property name */
  const gchar *property_name;
  /**Combobox of values available for the property*/
  GtkComboBox *values;
  /** Show property OSD */
  GtkDarktableToggleButton *osd;  
} 
dt_lib_camera_property_t;

typedef struct dt_lib_camera_t
{
  /** Gui part of the module */
  struct {
      GList *properties;    // a list of dt_lib_camera_property_t
  } gui;
  
  /** Data part of the module */
  struct  {
    dt_camctl_listener_t *listener;
  } data;
}
dt_lib_camera_t;



const char*
name ()
{
  return _("camera settings");
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
  return 998;
}

/** Add  a new property of camera to the gui */
dt_lib_camera_property_t *_lib_property_add_new(dt_lib_camera_t * lib, const gchar *label,const gchar *propertyname)
{
  if( dt_camctl_camera_property_exists(darktable.camctl,NULL,propertyname) )
  {
    const char *value;
    if( (value=dt_camctl_camera_property_get_first_choice(darktable.camctl,NULL,propertyname)) != NULL )
    { // We got a value for property lets construct the gui for the property and add values
      dt_lib_camera_property_t *prop = malloc(sizeof(dt_lib_camera_property_t));
      memset(prop,0,sizeof(dt_lib_camera_property_t));
      prop->label = GTK_LABEL(gtk_label_new(label));
      gtk_misc_set_alignment(GTK_MISC(prop->label ), 0.0, 0.5);
      prop->values=GTK_COMBO_BOX(gtk_combo_box_new());
      do
      {    
        gtk_combo_box_append_text(prop->values, value);
      } while( (value=dt_camctl_camera_property_get_next_choice(darktable.camctl,NULL,propertyname)) != NULL );
    }
  }
  return NULL;
}
  
/** Invoked when a value of a property is changed. */
static void _camera_property_value_changed(const dt_camera_t *camera,const char *name,const char *value,void *data)
{
//  dt_lib_camera_t *lib=(dt_lib_camera_t *)data;
}

/** Invoked when accesibility of a property is changed. */
static void _camera_property_accessibility_changed(const dt_camera_t *camera,const char *name,gboolean read_only,void *data)
{  
}

/** Listener callback from camera control when image are downloaded from camera. */
static void _camera_tethered_downloaded_callback(const dt_camera_t *camera,const char *filename,void *data)
{
  dt_job_t j;
  dt_captured_image_import_job_init(&j,filename);
  dt_control_add_job(darktable.control, &j);
}


#define BAR_HEIGHT 18
void 
gui_post_expose(dt_lib_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  // Draw infobar at top
  cairo_set_source_rgb (cr, .0,.0,.0);
  cairo_rectangle(cr, 0, 0, width, BAR_HEIGHT);
  cairo_fill (cr);
  
  // Draw control bar at bottom
  cairo_set_source_rgb (cr, .0,.0,.0);
  cairo_rectangle(cr, 0, height-BAR_HEIGHT, width, BAR_HEIGHT);
  cairo_fill (cr);
}

void
gui_init (dt_lib_module_t *self)
{
  self->widget = gtk_vbox_new(TRUE, 5);
  self->data = malloc(sizeof(dt_lib_camera_t));
  memset(self->data,0,sizeof(dt_lib_camera_t));
  
  // Setup lib data
  dt_lib_camera_t *lib=self->data;
  lib->data.listener = malloc(sizeof(dt_camctl_listener_t));
  memset(lib->data.listener,0,sizeof(dt_camctl_listener_t));
  lib->data.listener->data=lib;
  lib->data.listener->image_downloaded=_camera_tethered_downloaded_callback;
  lib->data.listener->camera_property_value_changed=_camera_property_value_changed;
  lib->data.listener->camera_property_accessibility_changed=_camera_property_accessibility_changed;
  
  
  // Setup gui
  self->widget = gtk_vbox_new(FALSE, 5);
  GtkBox *hbox, *vbox1, *vbox2;
  
  // Camera settings
  dt_lib_camera_property_t *prop;
  
  //gtk_box_pack_start(GTK_BOX(self->widget), dtgtk_label_new("session settings",DARKTABLE_LABEL_TAB|DARKTABLE_LABEL_ALIGN_RIGHT), TRUE, TRUE, 0);
  hbox = GTK_BOX(gtk_hbox_new(FALSE, 5));
  vbox1 = GTK_BOX(gtk_vbox_new(TRUE, 5));
  vbox2 = GTK_BOX(gtk_vbox_new(TRUE, 5));
  
  if( (prop=_lib_property_add_new(lib, _("program"),"exp.program"))!=NULL )
  {
    gtk_box_pack_start(vbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(vbox2, GTK_WIDGET(prop->values), TRUE, TRUE, 0);
  }
 
  if( (prop=_lib_property_add_new(lib, _("aperture"),"f-number"))!=NULL )
  {
    gtk_box_pack_start(vbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(vbox2, GTK_WIDGET(prop->values), TRUE, TRUE, 0);
  }
 
  if( (prop=_lib_property_add_new(lib, _("shutter"),"shuttertime2"))!=NULL )
  {
    gtk_box_pack_start(vbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(vbox2, GTK_WIDGET(prop->values), TRUE, TRUE, 0);
  }
 
  if( (prop=_lib_property_add_new(lib, _("iso"),"iso"))!=NULL)
  {
    gtk_box_pack_start(vbox1, GTK_WIDGET(prop->label), TRUE, TRUE, 0);
    gtk_box_pack_start(vbox2, GTK_WIDGET(prop->values), TRUE, TRUE, 0);
  }
  
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(vbox1), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(vbox2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  
  
  // Register listener 
  dt_camctl_register_listener(darktable.camctl,lib->data.listener);
  dt_camctl_tether_mode(darktable.camctl,NULL,TRUE);
}

void
gui_cleanup (dt_lib_module_t *self)
{
}

