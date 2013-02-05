/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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
#include "common/debug.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/image_cache.h"
#include "develop/develop.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "gui/draw.h"

#ifdef HAVE_UNITY
#  include <unity/unity/unity.h>
#endif
#ifdef MAC_INTEGRATION
#   include <gtkosxapplication.h>
#endif

DT_MODULE(1)

#define DT_MODULE_LIST_SPACING 2

GStaticMutex _lib_backgroundjobs_mutex = G_STATIC_MUTEX_INIT;

typedef struct dt_bgjob_t
{
  uint32_t type;
  GtkWidget *widget,*progressbar,*label;
#ifdef HAVE_UNITY
  UnityLauncherEntry *darktable_launcher;
#endif
} dt_bgjob_t;

typedef struct dt_lib_backgroundjobs_t
{
  GtkWidget *jobbox;
  GHashTable *jobs;
}
dt_lib_backgroundjobs_t;

/* proxy function for creating a ui bgjob plate */
static const guint *_lib_backgroundjobs_create(dt_lib_module_t *self,int type,const gchar *message);
/* proxy function for destroying a ui bgjob plate */
static void _lib_backgroundjobs_destroy(dt_lib_module_t *self, const guint *key);
/* proxy function for assigning and set cancel job for a ui bgjob plate*/
static void _lib_backgroundjobs_set_cancellable(dt_lib_module_t *self, const guint *key, struct dt_job_t *job);
/* proxy function for setting the progress of a ui bgjob plate */
static void _lib_backgroundjobs_progress(dt_lib_module_t *self, const guint *key, double progress);
/* callback when cancel job button is pushed  */
static void _lib_backgroundjobs_cancel_callback(GtkWidget *w, gpointer user_data);

const char* name()
{
  return _("background jobs");
}

uint32_t views()
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING | DT_VIEW_DARKROOM;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_LEFT_BOTTOM;
}

int position()
{
  return 1;
}

int expandable()
{
  return 0;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_backgroundjobs_t *d = (dt_lib_backgroundjobs_t *)g_malloc(sizeof(dt_lib_backgroundjobs_t));
  memset(d,0,sizeof(dt_lib_backgroundjobs_t));
  self->data = (void *)d;

  d->jobs = g_hash_table_new(g_direct_hash,g_direct_equal);

  /* initialize base */
  self->widget = d->jobbox = gtk_vbox_new(FALSE, 0);
  gtk_widget_set_no_show_all(self->widget, TRUE);
  gtk_container_set_border_width(GTK_CONTAINER(self->widget), 5);

  /* setup proxy */
  darktable.control->proxy.backgroundjobs.module = self;
  darktable.control->proxy.backgroundjobs.create = _lib_backgroundjobs_create;
  darktable.control->proxy.backgroundjobs.destroy = _lib_backgroundjobs_destroy;
  darktable.control->proxy.backgroundjobs.progress = _lib_backgroundjobs_progress;
  darktable.control->proxy.backgroundjobs.set_cancellable = _lib_backgroundjobs_set_cancellable;
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* lets kill proxy */
  darktable.control->proxy.backgroundjobs.module = NULL;

  g_free(self->data);
  self->data = NULL;
}

static const guint * _lib_backgroundjobs_create(dt_lib_module_t *self,int type,const gchar *message)
{
  dt_lib_backgroundjobs_t *d = (dt_lib_backgroundjobs_t *)self->data;

  /* lets make this threadsafe */
  gboolean i_own_lock = dt_control_gdk_lock();

  /* initialize a new job */
  dt_bgjob_t *j=(dt_bgjob_t*)g_malloc(sizeof(dt_bgjob_t));
  j->type = type;
  j->widget = gtk_event_box_new();

  guint *key = g_malloc(sizeof(guint));
  *key = g_direct_hash((gconstpointer)j);

  /* create in hash out of j pointer*/
  g_hash_table_insert(d->jobs, key, j);

  /* intialize the ui elements for job */
  gtk_widget_set_name (GTK_WIDGET (j->widget), "background_job_eventbox");
  GtkBox *vbox = GTK_BOX (gtk_vbox_new (FALSE,0));
  GtkBox *hbox = GTK_BOX (gtk_hbox_new (FALSE,0));
  gtk_container_set_border_width (GTK_CONTAINER(vbox),2);
  gtk_container_add (GTK_CONTAINER(j->widget), GTK_WIDGET(vbox));

  /* add job label */
  j->label = gtk_label_new(message);
  gtk_misc_set_alignment(GTK_MISC(j->label), 0.0, 0.5);
  gtk_box_pack_start( GTK_BOX( hbox ), GTK_WIDGET(j->label), TRUE, TRUE, 0);
  gtk_box_pack_start( GTK_BOX( vbox ), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  /* use progressbar ? */
  if (type == 0)
  {
    j->progressbar = gtk_progress_bar_new();
    gtk_box_pack_start( GTK_BOX( vbox ), j->progressbar, TRUE, FALSE, 2);

#ifdef HAVE_UNITY
    j->darktable_launcher = unity_launcher_entry_get_for_desktop_id("darktable.desktop");
    unity_launcher_entry_set_progress( j->darktable_launcher, 0.0 );
    unity_launcher_entry_set_progress_visible( j->darktable_launcher, TRUE );
#endif
  }

  /* lets show jobbox if its hidden */
  gtk_box_pack_start(GTK_BOX(d->jobbox), j->widget, TRUE, FALSE, 1);
  gtk_box_reorder_child(GTK_BOX(d->jobbox), j->widget, 1);
  gtk_widget_show_all(j->widget);
  gtk_widget_show(d->jobbox);

  if(i_own_lock) dt_control_gdk_unlock();
  return key;
}

static void _lib_backgroundjobs_destroy(dt_lib_module_t *self, const guint *key)
{
  dt_lib_backgroundjobs_t *d = (dt_lib_backgroundjobs_t*)self->data;

  gboolean i_own_lock = dt_control_gdk_lock();

  dt_bgjob_t *j = (dt_bgjob_t*)g_hash_table_lookup(d->jobs, key);
  if(j)
  {
    g_hash_table_remove(d->jobs, key);

    /* remove job widget from jobbox */
    if(GTK_IS_WIDGET(j->widget))
      gtk_container_remove(GTK_CONTAINER(d->jobbox),j->widget);

    /* if jobbox is empty lets hide */
    if(g_list_length(gtk_container_get_children(GTK_CONTAINER(d->jobbox)))==0)
      gtk_widget_hide(d->jobbox);

    /* free allocted mem */
    g_free(j);
    g_free((guint*)key);
  }
  if(i_own_lock) dt_control_gdk_unlock();
}

static void _lib_backgroundjobs_cancel_callback(GtkWidget *w, gpointer user_data)
{
  dt_job_t *job=(dt_job_t *)user_data;
  dt_control_job_cancel(job);
}

static void _lib_backgroundjobs_set_cancellable(dt_lib_module_t *self, const guint *key, struct dt_job_t *job)
{
  if(!darktable.control->running) return;
  gboolean i_own_lock = dt_control_gdk_lock();

  dt_lib_backgroundjobs_t *d = (dt_lib_backgroundjobs_t*)self->data;

  dt_bgjob_t *j = (dt_bgjob_t*)g_hash_table_lookup(d->jobs, key);
  if (j)
  {
    GtkWidget *w=j->widget;
    GtkBox *hbox = GTK_BOX (g_list_nth_data (gtk_container_get_children (GTK_CONTAINER ( gtk_bin_get_child (GTK_BIN (w) ) ) ), 0));
    GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_cancel,CPF_STYLE_FLAT);
    gtk_widget_set_size_request(button,17,17);
    g_signal_connect (G_OBJECT (button), "clicked", G_CALLBACK (_lib_backgroundjobs_cancel_callback), (gpointer)job);
    gtk_box_pack_start (hbox, GTK_WIDGET(button), FALSE, FALSE, 0);
    gtk_widget_show_all(button);
  }

  if(i_own_lock) dt_control_gdk_unlock();
}


static void _lib_backgroundjobs_progress(dt_lib_module_t *self, const guint *key, double progress)
{
  if(!darktable.control->running) return;
  dt_lib_backgroundjobs_t *d = (dt_lib_backgroundjobs_t*)self->data;
  gboolean i_own_lock = dt_control_gdk_lock();

  dt_bgjob_t *j = (dt_bgjob_t*)g_hash_table_lookup(d->jobs, key);
  if(j)
  {
    /* check if progress is above 1.0 and destroy bgjob if finished */
    /* FIXME: actually we are having some rounding issues, where the */
    /* FIXME: last item doesn't bring to total to 1.0 flat */
    /* FIXME: so this is why we have the ugly kludge below */
    if (progress > 0.999999)
    {
      if (GTK_IS_WIDGET(j->widget))
        gtk_container_remove( GTK_CONTAINER(d->jobbox), j->widget );

#ifdef HAVE_UNITY
      unity_launcher_entry_set_progress( j->darktable_launcher, 1.0 );
      unity_launcher_entry_set_progress_visible( j->darktable_launcher, FALSE );
#endif
#ifdef MAC_INTEGRATION
#ifdef GTK_TYPE_OSX_APPLICATION
      gtk_osxapplication_attention_request(g_object_new(GTK_TYPE_OSX_APPLICATION, NULL), INFO_REQUEST);
#else
      gtkosx_application_attention_request(g_object_new(GTKOSX_TYPE_APPLICATION, NULL), INFO_REQUEST);
#endif
#endif

      /* hide jobbox if theres no jobs left */
      if (g_list_length(gtk_container_get_children(GTK_CONTAINER(d->jobbox))) == 0 )
        gtk_widget_hide(d->jobbox);
    }
    else
    {
      if( j->type == 0 )
        gtk_progress_bar_set_fraction( GTK_PROGRESS_BAR(j->progressbar), progress );

#ifdef HAVE_UNITY
      unity_launcher_entry_set_progress( j->darktable_launcher, progress );
#endif
    }
  }

  if(i_own_lock) dt_control_gdk_unlock();
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
