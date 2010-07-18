/*
    This file is part of darktable,
    copyright (c) 2009--2010 henrik andersson

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

#include <string.h>
#include <glade/glade.h>

#include "common/darktable.h"
#include "gui/gtk.h"
#include "gui/background_jobs.h"

GStaticMutex _gui_background_mutex = G_STATIC_MUTEX_INIT;

GtkLabel *_gui_background_jobs_get_label( GtkWidget *w ) {
  return g_list_nth_data( gtk_container_get_children( GTK_CONTAINER( gtk_bin_get_child ( GTK_BIN( w ) ) ) ), 0);
}

GtkProgressBar *_gui_background_jobs_get_progressbar( GtkWidget *w ) {
  return g_list_nth_data( gtk_container_get_children( GTK_CONTAINER( gtk_bin_get_child ( GTK_BIN( w ) ) ) ), 1);
}

const dt_gui_job_t *dt_gui_background_jobs_new(dt_gui_job_type_t type, const gchar *message)
{
  gdk_threads_enter();
  dt_gui_job_t *j=g_malloc(sizeof(dt_gui_job_t));
  memset(j,0,sizeof( dt_gui_job_t ) );
  j->message = g_strdup( message );
  j->progress = 0;
  j->type = type;
  j->widget = gtk_event_box_new();
  gtk_widget_set_name(GTK_WIDGET( j->widget ), "background_job_eventbox");
  GtkBox *vbox = GTK_BOX( gtk_vbox_new(FALSE,0) );
  gtk_container_set_border_width(GTK_CONTAINER(vbox),4);
  gtk_container_add( GTK_CONTAINER( j->widget ), GTK_WIDGET( vbox ) );
  GtkLabel *label=GTK_LABEL(gtk_label_new( message ));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start( GTK_BOX( vbox ), GTK_WIDGET(label), TRUE, TRUE, 2);
  
  if( type == DT_JOB_PROGRESS )
    gtk_box_pack_start( GTK_BOX( vbox ), gtk_progress_bar_new( ), TRUE, FALSE, 0);
  
  //g_static_mutex_lock ( &_gui_background_mutex );
  
  // add job widget to 
  GtkWidget *w = glade_xml_get_widget( darktable.gui->main_window, "jobs_content_box" );
  gtk_box_pack_start( GTK_BOX( w ), j->widget, TRUE, FALSE, 2);
  gtk_box_reorder_child ( GTK_BOX( w ), j->widget, 0);
  gtk_widget_show_all( j->widget );
  
  w = glade_xml_get_widget( darktable.gui->main_window, "jobs_list_expander" );
  gtk_expander_set_expanded( GTK_EXPANDER( w ), TRUE );
 // g_static_mutex_unlock ( &_gui_background_mutex );
  gdk_threads_leave();
  return j;
}

void dt_gui_background_jobs_set_message(const dt_gui_job_t *j,const gchar *message)
{
  gdk_threads_enter();
 
 // g_static_mutex_lock ( &_gui_background_mutex );
  gtk_label_set_text( _gui_background_jobs_get_label( j->widget ), j->message );
  //g_static_mutex_unlock( &_gui_background_mutex );
  gdk_threads_leave();
}

void dt_gui_background_jobs_set_progress(const dt_gui_job_t *j,double progress)
{
  gdk_threads_enter();
  
  // g_static_mutex_lock ( &_gui_background_mutex );
 
  if( progress >= 1.0 ) {	// job is finished free and destroy the widget..
    
    GtkWidget *w = glade_xml_get_widget( darktable.gui->main_window, "jobs_content_box" );
    gtk_container_remove( GTK_CONTAINER( w ), j->widget );
    g_free( (dt_gui_job_t*)j );
    
    if( g_list_length( gtk_container_get_children( GTK_CONTAINER( w) ) ) == 0 )
    { 
      // collapse job expander
      w = glade_xml_get_widget( darktable.gui->main_window, "jobs_list_expander" );
      gtk_expander_set_expanded( GTK_EXPANDER( w ), FALSE );
    }
    
  } else {
    if( j->type == DT_JOB_PROGRESS )
      gtk_progress_bar_set_fraction( _gui_background_jobs_get_progressbar( j->widget ), progress );
  }
  //g_static_mutex_unlock ( &_gui_background_mutex );
  gdk_threads_leave();
}

