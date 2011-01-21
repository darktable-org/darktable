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
#include <stdio.h>
#include <stdlib.h>
#include "gui/gtk.h"
#include "gui/devices.h"
#include "gui/camera_import_dialog.h"
#include "develop/develop.h"
#include "dtgtk/label.h"
#include "dtgtk/button.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "common/film.h"
#include "common/variables.h"
#include "common/camera_control.h"




static dt_camctl_listener_t _gui_camctl_listener;

static void _camctl_camera_disconnected_callback (const dt_camera_t *camera,void *data)
{
  dt_camctl_detect_cameras(darktable.camctl);
  gdk_threads_enter();
  dt_gui_devices_update();
  gdk_threads_leave();
}

static void _camctl_camera_control_status_callback(dt_camctl_status_t status,void *data)
{
  switch(status)
  {
    case CAMERA_CONTROL_BUSY:
    {
      //dt_control_log(_("camera control is busy."));
      GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "devices_expander_body");
      GList *child = gtk_container_get_children(GTK_CONTAINER(widget));
      if(child) 
        do
        {
          if( !(GTK_IS_TOGGLE_BUTTON(child->data)  && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(child->data))==TRUE) )
            gtk_widget_set_sensitive(GTK_WIDGET(child->data),FALSE);
        } while( (child=g_list_next(child)) );
    } break;
    
    case CAMERA_CONTROL_AVAILABLE:
    {
      //dt_control_log(_("camera control is available."));
      GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "devices_expander_body");
      GList *child = gtk_container_get_children(GTK_CONTAINER(widget));
      if(child) 
        do
        {
          gtk_widget_set_sensitive(GTK_WIDGET(child->data),TRUE);
        } while( (child=g_list_next(child)) );
    } break;
  }
}

static void scan_callback(GtkButton *button,gpointer data)  
{
  dt_camctl_detect_cameras(darktable.camctl);
  dt_gui_devices_update();
}

static void import_callback(GtkButton *button,gpointer data)  
{
  dt_camera_import_dialog_param_t *params=(dt_camera_import_dialog_param_t *)g_malloc(sizeof(dt_camera_import_dialog_param_t));
  memset( params, 0, sizeof(dt_camera_import_dialog_param_t));
  params->camera = (dt_camera_t*)data;
  
  dt_camera_import_dialog_new(params);
  if( params->result )
  {
    // Let's initialize a import job and put it on queue....
    gchar *path = g_build_path(G_DIR_SEPARATOR_S,params->basedirectory,params->subdirectory,(char *)NULL);
    dt_job_t j;
    dt_camera_import_job_init(&j,params->jobcode,path,params->filenamepattern,params->result,params->camera);
    dt_control_add_job(darktable.control, &j);
    g_free(path);
  }
  g_free(params);
}


static void tethered_callback(GtkToggleButton *button,gpointer data)  
{
  /* select camera to work with before switching mode */
  dt_camctl_select_camera(darktable.camctl, (dt_camera_t *)data);
  dt_conf_set_int( "plugins/capture/mode", DT_CAPTURE_MODE_TETHERED);
  dt_conf_set_int("plugins/capture/current_filmroll",-1);
  dt_ctl_switch_mode_to(DT_CAPTURE);
}



void dt_gui_devices_init() 
{
  memset(&_gui_camctl_listener,0,sizeof(dt_camctl_listener_t));
  _gui_camctl_listener.control_status = _camctl_camera_control_status_callback;
  _gui_camctl_listener.camera_disconnected = _camctl_camera_disconnected_callback;
  dt_camctl_register_listener( darktable.camctl, &_gui_camctl_listener );
  dt_gui_devices_update();
}

void dt_gui_devices_update()
{
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "devices_expander_body");
  GList *citem;
  
  // Lets clear all items in container...
  GList *item;
  if((item=gtk_container_get_children(GTK_CONTAINER(widget)))!=NULL)
    do {
      gtk_container_remove(GTK_CONTAINER(widget),GTK_WIDGET(item->data));
    } while((item=g_list_next(item))!=NULL);

  // Add the rescan button
  GtkButton *scan=GTK_BUTTON(gtk_button_new_with_label(_("scan for devices")));
  gtk_button_set_alignment(scan, 0.05, 0.5);
  gtk_object_set(GTK_OBJECT(scan), "tooltip-text", _("scan for newly attached devices"), (char *)NULL);
  g_signal_connect (G_OBJECT(scan), "clicked",G_CALLBACK (scan_callback), NULL);
  gtk_box_pack_start(GTK_BOX(widget),GTK_WIDGET(scan),TRUE,TRUE,0);
  gtk_box_pack_start(GTK_BOX(widget),GTK_WIDGET(gtk_label_new("")),TRUE,TRUE,0);
    
  uint32_t count=0;
  if( (citem = g_list_first (darktable.camctl->cameras))!=NULL) 
  {    
    // Add detected supported devices
    char buffer[512]={0};
    do
    {
      dt_camera_t *camera=(dt_camera_t *)citem->data;
      count++;
      // Add camera label
      GtkWidget *label = GTK_WIDGET (dtgtk_label_new (camera->model,DARKTABLE_LABEL_TAB|DARKTABLE_LABEL_ALIGN_LEFT));
      gtk_box_pack_start (GTK_BOX (widget),label,TRUE,TRUE,0);
      
      // Set summary if exists for tooltip
      if( camera->summary.text !=NULL && strlen(camera->summary.text) >0 ) 
      {
        gtk_object_set(GTK_OBJECT(label), "tooltip-text", camera->summary.text, (char *)NULL);
      }
      else
      {
        sprintf(buffer,_("device \"%s\" connected on port \"%s\"."),camera->model,camera->port);
        gtk_object_set(GTK_OBJECT(label), "tooltip-text", buffer, (char *)NULL);
      }
      
      // Add camera action buttons
      GtkWidget *ib=NULL,*tb=NULL;
      GtkWidget *vbx=gtk_vbox_new(FALSE,5);
      if( camera->can_import==TRUE )
        gtk_box_pack_start (GTK_BOX (vbx),(ib=gtk_button_new_with_label (_("import from camera"))),FALSE,FALSE,0);
      if( camera->can_tether==TRUE )
        gtk_box_pack_start (GTK_BOX (vbx),(tb=gtk_button_new_with_label (_("tethered shoot"))),FALSE,FALSE,0);
      
      if( ib ) {
        g_signal_connect (G_OBJECT (ib), "clicked",G_CALLBACK (import_callback), camera);
        gtk_button_set_alignment(GTK_BUTTON(ib), 0.05, 0.5);
      }
      if( tb ) {
        g_signal_connect (G_OBJECT (tb), "clicked",G_CALLBACK (tethered_callback), camera);
        gtk_button_set_alignment(GTK_BUTTON(tb), 0.05, 0.5);
      }
      gtk_box_pack_start (GTK_BOX (widget),vbx,FALSE,FALSE,0);
    } while ((citem=g_list_next (citem))!=NULL);
  } 
  
  if( count == 0 )
  { // No supported devices is detected lets notice user..
    gtk_box_pack_start(GTK_BOX(widget),gtk_label_new(_("no supported devices found")),TRUE,TRUE,0);
  }
  gtk_widget_show_all(widget);
}



