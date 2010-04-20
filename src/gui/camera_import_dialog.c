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

#include "develop/develop.h"
#include "control/conf.h"
#include "common/camera_control.h"
#include "dtgtk/button.h"
#include "gui/camera_import_dialog.h"

/*
  
  g_object_ref(model); // Make sure the model stays with us after the tree view unrefs it 

  gtk_tree_view_set_model(GTK_TREE_VIEW(view), NULL); // Detach model from view 

  ... insert a couple of thousand rows ...

  gtk_tree_view_set_model(GTK_TREE_VIEW(view), model); // Re-attach model to view 

  g_object_unref(model);
  
*/

typedef struct _camera_import_dialog_t {
  GtkWidget *dialog;
  
  GtkWidget *notebook;
  
  struct {
    GtkWidget *page;
    GtkWidget *jobname;
    GtkWidget *treeview;
    GtkWidget *info;  
  } import;
  
  struct {
    GtkWidget *page;
    GtkWidget *basedirectory;
    GtkWidget *subdirectory;
    GtkWidget *namepattern;
  } settings;
  
  GtkListStore *store;

  const dt_camera_t *camera;

  GList **result;
}
_camera_import_dialog_t;

GtkWidget *_camera_import_gconf_widget(gchar *label,gchar *confstring) 
{
  GtkWidget *vbox,*hbox;
  vbox=GTK_WIDGET(gtk_vbox_new(FALSE,0));
  hbox=GTK_WIDGET(gtk_hbox_new(FALSE,0));
  g_object_set_data(G_OBJECT(vbox),"gconf:string",confstring);
  
  GtkWidget *entry = gtk_entry_new();
  if( dt_conf_get_string (confstring) )
    gtk_entry_set_text( GTK_ENTRY( entry ),dt_conf_get_string (confstring));
  gtk_box_pack_start(GTK_BOX(hbox),GTK_WIDGET(entry),TRUE,TRUE,0);
  
  GtkWidget *button=dtgtk_button_new(dtgtk_cairo_paint_store,0);
  g_object_set(button,"tooltip-text",_("store value as default"),NULL);
  gtk_widget_set_size_request(button,13,13);
  gtk_box_pack_start(GTK_BOX(hbox),button,FALSE,FALSE,0);
  
  button=dtgtk_button_new(dtgtk_cairo_paint_reset,0);
  g_object_set(button,"tooltip-text",_("reset value to default"),NULL);
  gtk_widget_set_size_request(button,13,13);
  gtk_box_pack_start(GTK_BOX(hbox),button,FALSE,FALSE,0);
  
  GtkWidget *l=gtk_label_new(label);
  gtk_misc_set_alignment(GTK_MISC(l), 0.0, 0.0);
  gtk_box_pack_start(GTK_BOX(vbox),l,FALSE,FALSE,0);
  
  gtk_box_pack_start(GTK_BOX(vbox),GTK_WIDGET(hbox),FALSE,FALSE,0);
  
  return vbox;
}

void _camera_import_dialog_new(_camera_import_dialog_t *data) {
  data->dialog=gtk_dialog_new_with_buttons(_("import images from camera"),NULL,GTK_DIALOG_MODAL,_("cancel"),GTK_RESPONSE_NONE,_("import"),GTK_RESPONSE_ACCEPT,NULL);
  GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (data->dialog));
  
  // List - setup store
  data->store = gtk_list_store_new (2,GDK_TYPE_PIXBUF,G_TYPE_STRING);
  
  // IMPORT PAGE
  data->import.page=gtk_vbox_new(FALSE,5);
  gtk_container_set_border_width(GTK_CONTAINER(data->import.page),5);
  
  // Top info
  data->import.info=gtk_label_new( _("please wait while prefetcing thumbnails of images from camera...") );
  gtk_label_set_single_line_mode( GTK_LABEL(data->import.info) , FALSE );
  gtk_box_pack_start(GTK_BOX(data->import.page),data->import.info,FALSE,FALSE,0);
  
  // n/v
//  GtkBox *hbox=GTK_BOX(gtk_hbox_new(FALSE,2));

  data->import.jobname=_camera_import_gconf_widget(_("jobcode"),"capture/camera/import/jobcode");
  gtk_box_pack_start(GTK_BOX(data->import.page),GTK_WIDGET(data->import.jobname),FALSE,FALSE,0);
  
  
  // Create the treview with list model data store
  data->import.treeview=gtk_scrolled_window_new(NULL,NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(data->import.treeview),GTK_POLICY_NEVER,GTK_POLICY_ALWAYS);
  
  gtk_container_add(GTK_CONTAINER(data->import.treeview), gtk_tree_view_new());
  GtkTreeView *treeview=GTK_TREE_VIEW(gtk_bin_get_child(GTK_BIN(data->import.treeview)));  
  
  GtkCellRenderer *renderer = gtk_cell_renderer_pixbuf_new( );
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes( _("thumbnail") , renderer,"pixbuf",0, NULL);
  gtk_tree_view_append_column( treeview , column);
  
  renderer = gtk_cell_renderer_text_new( );
  column = gtk_tree_view_column_new_with_attributes( _("storage file"), renderer, "text", 1, NULL);
  gtk_tree_view_append_column( treeview , column);
  gtk_tree_view_column_set_expand( column, TRUE);
  
  
  GtkTreeSelection *selection = gtk_tree_view_get_selection(treeview);
  gtk_tree_selection_set_mode(selection,GTK_SELECTION_MULTIPLE);
  
  gtk_tree_view_set_model(treeview,GTK_TREE_MODEL(data->store));
  
  gtk_box_pack_start(GTK_BOX(data->import.page),data->import.treeview,TRUE,TRUE,0);
  
  
  // SETTINGS PAGE
  data->settings.page=gtk_vbox_new(FALSE,5);
  gtk_container_set_border_width(GTK_CONTAINER(data->import.page),5);
  
  
  data->settings.basedirectory=_camera_import_gconf_widget(_("storage directory"),"capture/camera/storage/basedirectory");
  gtk_box_pack_start(GTK_BOX(data->settings.page),GTK_WIDGET(data->settings.basedirectory),FALSE,FALSE,0);
  
  data->settings.subdirectory=_camera_import_gconf_widget(_("directory structure"),"capture/camera/storage/subpath");
  gtk_box_pack_start(GTK_BOX(data->settings.page),GTK_WIDGET(data->settings.subdirectory),FALSE,FALSE,0);
  
  
  data->settings.namepattern=_camera_import_gconf_widget(_("filename structure"),"capture/camera/storage/namepattern");
  gtk_box_pack_start(GTK_BOX(data->settings.page),GTK_WIDGET(data->settings.namepattern),FALSE,FALSE,0);
  
  
  
  // THE NOTEBOOOK
  data->notebook=gtk_notebook_new();
  gtk_notebook_append_page(GTK_NOTEBOOK(data->notebook),data->import.page,gtk_label_new(_("import images")));
  gtk_notebook_append_page(GTK_NOTEBOOK(data->notebook),data->settings.page,gtk_label_new(_("import settings")));
  
  // end
  gtk_box_pack_start(GTK_BOX(content),data->notebook,TRUE,TRUE,0);
  gtk_widget_set_size_request(content,400,400);

}

void _camera_storage_image_filename(const dt_camera_t *camera,const char *filename,CameraFile *preview,void *user_data) 
{
  _camera_import_dialog_t *data=(_camera_import_dialog_t*)user_data;
  GtkTreeIter iter;
  const char *img;
  unsigned long size;
  GdkPixbuf *pixbuf=NULL;
  GdkPixbuf *thumb=NULL;
    
  if( preview )
  {
    gp_file_get_data_and_size(preview, &img, &size);
    if( size > 0 )
    { // we got preview image data lets create a pixbuf from image blob
      GError *err=NULL;
      GInputStream *stream;
      if( (stream = g_memory_input_stream_new_from_data(img, size,NULL)) !=NULL)
          pixbuf = gdk_pixbuf_new_from_stream( stream, NULL, &err );
        
    }
      
    if(pixbuf)
    { // Scale pixbuf to a thumbnail
      double sw=gdk_pixbuf_get_width( pixbuf );
      double scale=75.0/gdk_pixbuf_get_height( pixbuf );
      thumb = gdk_pixbuf_scale_simple( pixbuf, sw*scale,75 , GDK_INTERP_BILINEAR );
    }
  }
  
  gtk_list_store_append(data->store,&iter);
  gtk_list_store_set(data->store,&iter,0,thumb,1,filename,-1);
  if(pixbuf) g_object_unref(pixbuf);
  if(thumb) g_object_ref(thumb);
}


void _camera_import_dialog_run(_camera_import_dialog_t *data) 
{
  gtk_widget_show_all(data->dialog);
  
  // Populate store
 
  // Setup a listener for previwes of all files on camera
  // then initiate fetch of all previews from camera
  if(data->camera!=NULL)
  {
    dt_camctl_listener_t listener={0};
    listener.data=data;
    listener.camera_storage_image_filename=_camera_storage_image_filename;
    dt_camctl_register_listener(darktable.camctl,&listener);
    dt_camctl_get_previews(darktable.camctl,data->camera);
    dt_camctl_unregister_listener(darktable.camctl,&listener);
  }
  
  // Select all images as default
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(gtk_bin_get_child(GTK_BIN(data->import.treeview))));
  gtk_tree_selection_select_all(selection);
  
  // Lets run dialog
  gtk_label_set_text(GTK_LABEL(data->import.info),_("select the images from the list below that you want to import into a new filmroll"));
  gint result = gtk_dialog_run (GTK_DIALOG (data->dialog));
  if( result == GTK_RESPONSE_ACCEPT) 
  {
    GtkTreeIter iter;
    // Now build up result from store into GList **result
    GtkTreeModel *model=GTK_TREE_MODEL(data->store);
    GList *sp= gtk_tree_selection_get_selected_rows(selection,&model);
    if( sp )
    {
        do
        {
          GValue value;
          g_value_init(&value, G_TYPE_STRING);
          gtk_tree_model_get_iter(GTK_TREE_MODEL (data->store),&iter,(GtkTreePath*)sp->data);
          gtk_tree_model_get_value(GTK_TREE_MODEL (data->store),&iter,1,&value);
          *data->result=g_list_append(*data->result,g_strdup(g_value_get_string(&value)) );
          g_value_unset(&value);
        } while( (sp=g_list_next(sp)) );
    }
  }
  // Destory and quit 
  gtk_widget_destroy (data->dialog);
}

void dt_camera_import_dialog_new(GList **result,const dt_camera_t *cam)
{
  _camera_import_dialog_t data;
  data.result = result;
  data.camera=cam;
  _camera_import_dialog_new(&data);
  _camera_import_dialog_run(&data);
}