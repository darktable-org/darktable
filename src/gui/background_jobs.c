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
#include "control/control.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "dtgtk/label.h"
#include "gui/background_jobs.h"

GStaticMutex _gui_background_mutex = G_STATIC_MUTEX_INIT;


void dt_gui_background_jobs_init()
{
  GtkWidget *w = glade_xml_get_widget( darktable.gui->main_window, "jobs_content_box" );
  GtkWidget *label =  dtgtk_label_new (_("background jobs"), DARKTABLE_LABEL_TAB | DARKTABLE_LABEL_ALIGN_LEFT);
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start( GTK_BOX( w ), label, TRUE, TRUE, 0);
  gtk_widget_show(label);
  gtk_box_pack_start (GTK_BOX (w), gtk_vbox_new(FALSE,0),FALSE,FALSE,0);
  gtk_widget_hide_all( w );
}

static void
_cancel_job_clicked (GtkDarktableButton *button, gpointer user_data)
{
  dt_gui_job_t *j= (dt_gui_job_t *)user_data;
  if (j->job)
    dt_control_job_cancel(j->job);
}

void _gui_background_jobs_add_cancel( const dt_gui_job_t *j )
{
  GtkWidget *w=j->widget;
  GtkBox *hbox = GTK_BOX (g_list_nth_data (gtk_container_get_children (GTK_CONTAINER ( gtk_bin_get_child (GTK_BIN (w) ) ) ), 0));
  GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_cancel,CPF_STYLE_FLAT);
  gtk_widget_set_size_request(button,17,17);
  g_signal_connect (G_OBJECT (button), "clicked", G_CALLBACK (_cancel_job_clicked), (gpointer)j);
  gtk_box_pack_start (hbox, GTK_WIDGET(button), FALSE, FALSE, 0);
  gtk_widget_show_all(button);
}

GtkLabel *_gui_background_jobs_get_label( GtkWidget *w )
{
  // eventbox->vbox[0]->hbox[0] = label
  return g_list_nth_data(gtk_container_get_children( GTK_CONTAINER (g_list_nth_data( gtk_container_get_children( GTK_CONTAINER( gtk_bin_get_child ( GTK_BIN( w ) ) ) ), 0) ) ), 0);
}

GtkProgressBar *_gui_background_jobs_get_progressbar( GtkWidget *w )
{
  // eventbox->vbox[1] = progress
  return g_list_nth_data( gtk_container_get_children( GTK_CONTAINER( gtk_bin_get_child ( GTK_BIN( w ) ) ) ), 1);
}

void dt_gui_background_jobs_can_cancel(const dt_gui_job_t *j, dt_job_t *job)
{
  dt_gui_job_t *gjob=(dt_gui_job_t *)j;
  gjob->job = job;
  _gui_background_jobs_add_cancel(j);
}

const dt_gui_job_t *dt_gui_background_jobs_new(dt_gui_job_type_t type, const gchar *message)
{
  // we call gtk stuff, possibly from a worker thread:
  int needlock = !pthread_equal(pthread_self(),darktable.control->gui_thread);
  if(needlock) gdk_threads_enter();
  /* initialize a dt_gui_job */
  dt_gui_job_t *j=g_malloc(sizeof(dt_gui_job_t));
  memset(j,0,sizeof( dt_gui_job_t ) );
  j->message = g_strdup( message );
  j->progress = 0;
  j->type = type;
  j->widget = gtk_event_box_new();
  
  gtk_widget_set_name (GTK_WIDGET (j->widget), "background_job_eventbox");
  GtkBox *vbox = GTK_BOX (gtk_vbox_new (FALSE,0));
  GtkBox *hbox = GTK_BOX (gtk_hbox_new (FALSE,0));
  gtk_container_set_border_width (GTK_CONTAINER(vbox),2);
  gtk_container_add ( GTK_CONTAINER( j->widget ), GTK_WIDGET( vbox ) );
  
  /* add job label */
  GtkLabel *label=GTK_LABEL(gtk_label_new( message ));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start( GTK_BOX( hbox ), GTK_WIDGET(label), TRUE, TRUE, 0);
  gtk_box_pack_start( GTK_BOX( vbox ), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  
  if( type == DT_JOB_PROGRESS )
    gtk_box_pack_start( GTK_BOX( vbox ), gtk_progress_bar_new( ), TRUE, FALSE, 2);

  /* If the backgrounds jobs are hidden lets show it... */
  GtkWidget *w = glade_xml_get_widget (darktable.gui->main_window, "jobs_content_box" );
  GtkWidget *jobbox = g_list_nth_data (gtk_container_get_children (GTK_CONTAINER (w)),1);
  gtk_box_pack_start (GTK_BOX (jobbox), j->widget, TRUE, FALSE, 1);
  gtk_box_reorder_child ( GTK_BOX (jobbox), j->widget, 1);
  gtk_widget_show_all( j->widget );
  gtk_widget_show (jobbox);
  gtk_widget_show (w);

  if(needlock) gdk_threads_leave();
  return j;
}

void dt_gui_background_jobs_destroy(const dt_gui_job_t *j) 
{
  int needlock = !pthread_equal(pthread_self(),darktable.control->gui_thread);
  if(needlock) gdk_threads_enter();
  // remove widget if not already removed from jobcontainer...
  GtkWidget *w = glade_xml_get_widget (darktable.gui->main_window, "jobs_content_box");
  GtkWidget *jobbox = g_list_nth_data (gtk_container_get_children (GTK_CONTAINER (w)),1);
  
  if (j->widget && GTK_IS_WIDGET(j->widget))
  {
    gtk_container_remove (GTK_CONTAINER (jobbox), j->widget);
    ((dt_gui_job_t *)j)->widget = NULL;
  }
  g_free ((dt_gui_job_t*)j);
  if(needlock) gdk_threads_leave();
}


void dt_gui_background_jobs_set_message(const dt_gui_job_t *j,const gchar *message)
{
  if(!darktable.control->running) return;
  int needlock = !pthread_equal(pthread_self(),darktable.control->gui_thread);
  if(needlock) gdk_threads_enter();
 
 // g_static_mutex_lock ( &_gui_background_mutex );
  gtk_label_set_text( _gui_background_jobs_get_label( j->widget ), j->message );
  //g_static_mutex_unlock( &_gui_background_mutex );
  if(needlock) gdk_threads_leave();
}

void dt_gui_background_jobs_set_progress(const dt_gui_job_t *j,double progress)
{
  if(!darktable.control->running) return;
  int needlock = !pthread_equal(pthread_self(),darktable.control->gui_thread);
  if(needlock) gdk_threads_enter();
  
  // g_static_mutex_lock ( &_gui_background_mutex );
 
  if( progress >= 1.0 )
  {	// job is finished free and destroy the widget..
    GtkWidget *w = glade_xml_get_widget( darktable.gui->main_window, "jobs_content_box" );
    GtkWidget *jobbox = g_list_nth_data (gtk_container_get_children (GTK_CONTAINER (w)),1);
    if (j->widget && GTK_IS_WIDGET(j->widget))
    {
      gtk_container_remove( GTK_CONTAINER( jobbox ), j->widget );
      // const cast.
      ((dt_gui_job_t *)j)->widget = NULL;
    }
    
    // hide box if we are last active job..
    if( g_list_length( gtk_container_get_children( GTK_CONTAINER (jobbox) ) ) == 0 )
      gtk_widget_hide( w );
  }
  else
  {
    if( j->type == DT_JOB_PROGRESS )
      gtk_progress_bar_set_fraction( _gui_background_jobs_get_progressbar( j->widget ), progress );
  }
  //g_static_mutex_unlock ( &_gui_background_mutex );
  if(needlock) gdk_threads_leave();
}

