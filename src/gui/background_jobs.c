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


void dt_gui_background_jobs_init( dt_gui_jobs_t *j)
{
  j->jobs=NULL;
}

void dt_gui_background_jobs_cleanup( dt_gui_jobs_t *j)
{
}

GtkLabel *_gui_background_jobs_get_label( GtkWidget *w ) {
  return g_list_nth_data( gtk_container_get_children( GTK_CONTAINER( gtk_bin_get_child ( GTK_BIN( w ) ) ) ), 0);
}

GtkProgressBar *_gui_background_jobs_get_progressbar( GtkWidget *w ) {
  return g_list_nth_data( gtk_container_get_children( GTK_CONTAINER( gtk_bin_get_child ( GTK_BIN( w ) ) ) ), 1);
}

const dt_gui_job_t *dt_gui_background_jobs_new(const gchar *message)
{
  dt_gui_job_t *j=g_malloc(sizeof(dt_gui_job_t));
  memset(j,0,sizeof( dt_gui_job_t ) );
  j->message = g_strdup( message );
  j->progress = 0;
  
  j->widget = gtk_event_box_new();
  gtk_widget_set_name(GTK_WIDGET( j->widget ), "background_job_eventbox");
  GtkBox *vbox = GTK_BOX( gtk_vbox_new(FALSE,0) );
  gtk_container_set_border_width(GTK_CONTAINER(vbox),4);
  gtk_container_add( GTK_CONTAINER( j->widget ), GTK_WIDGET( vbox ) );
  GtkLabel *label=GTK_LABEL(gtk_label_new( message ));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start( GTK_BOX( vbox ), GTK_WIDGET(label), TRUE, TRUE, 2);
  gtk_box_pack_start( GTK_BOX( vbox ), gtk_progress_bar_new( ), TRUE, FALSE, 0);
  
  // add job widget to 
  GtkWidget *w = glade_xml_get_widget( darktable.gui->main_window, "jobs_content_box" );
  gtk_box_pack_start( GTK_BOX( w ), j->widget, TRUE, FALSE, 2);
  gtk_box_reorder_child ( GTK_BOX( w ), j->widget, 0);
  gtk_widget_show_all( j->widget );
  gtk_expander_set_expanded( GTK_EXPANDER( w ), TRUE );
  return j;
}

void dt_gui_background_jobs_set_message(const dt_gui_job_t *j,const gchar *message)
{
  gtk_label_set_text( _gui_background_jobs_get_label( j->widget ), j->message );
}

void dt_gui_background_jobs_set_progress(const dt_gui_job_t *j,double progress)
{
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
    
  } else
    gtk_progress_bar_set_fraction( _gui_background_jobs_get_progressbar( j->widget ), progress );
}

