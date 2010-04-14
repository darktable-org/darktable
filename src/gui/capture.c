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

#include "gui/gtk.h"
#include "gui/capture.h"
#include "develop/develop.h"
#include "dtgtk/label.h"
#include "common/camera_control.h"

static void detect_source_callback(GtkButton *button,gpointer user_data)  
{
  dt_camctl_detect_cameras(darktable.camctl);
  dt_gui_capture_update();
}

void dt_gui_capture_update()
{
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "capture_expander_body");
  GList *citem;
  if( (citem=g_list_first(darktable.camctl->cameras))!=NULL) 
  {
    // Lets clear all items in container...
    GList *item;
    if((item=gtk_container_get_children(GTK_CONTAINER(widget)))!=NULL)
      do {
        gtk_container_remove(GTK_CONTAINER(widget),GTK_WIDGET(item->data));
      } while((item=g_list_next(item))!=NULL);
    
    // Add detect button
    GtkWidget *button=gtk_button_new_with_label(_("detect sources"));
    g_signal_connect (G_OBJECT(button), "clicked",G_CALLBACK (detect_source_callback), NULL);
    gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("scan and detect sources available for capture"), NULL);
    gtk_box_pack_start(GTK_BOX(widget),button,FALSE,FALSE,0);
      
    // Add detected capture sources
    char buffer[512]={0};
    do
    {
      dt_camera_t *camera=(dt_camera_t *)citem->data;
      
      // Add camera label
      GtkWidget *label=GTK_WIDGET(dtgtk_label_new(camera->model,DARKTABLE_LABEL_TAB|DARKTABLE_LABEL_ALIGN_RIGHT));
      gtk_box_pack_start(GTK_BOX(widget),label,TRUE,TRUE,0);
      sprintf(buffer,_("Camera %s connected on port %s"),camera->model,camera->port);
      gtk_object_set(GTK_OBJECT(label), "tooltip-text", buffer, NULL);
      
      // Add camera action buttons
      if( camera->can_import==TRUE )
        gtk_box_pack_start(GTK_BOX(widget),gtk_button_new_with_label(_("import")),FALSE,FALSE,0);
      if( camera->can_tether==TRUE )
        gtk_box_pack_start(GTK_BOX(widget),gtk_button_new_with_label(_("tethered shoot")),FALSE,FALSE,0);
            
    } while((citem=g_list_next(citem))!=NULL);
  }
}

