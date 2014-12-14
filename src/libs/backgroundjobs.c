/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.
    copyright (c) 2014 tobias ellinghaus.

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
#include "control/progress.h"
#include "common/image_cache.h"
#include "develop/develop.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "gui/draw.h"

DT_MODULE(1)

typedef struct dt_lib_backgroundjob_element_t
{
  GtkWidget *widget, *progressbar, *hbox;
} dt_lib_backgroundjob_element_t;

/* proxy functions */
static void *_lib_backgroundjobs_added(dt_lib_module_t *self, gboolean has_progress_bar, const gchar *message);
static void _lib_backgroundjobs_destroyed(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance);
static void _lib_backgroundjobs_cancellable(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance,
                                            dt_progress_t *progress);
static void _lib_backgroundjobs_updated(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance,
                                        double value);


const char *name()
{
  return _("background jobs");
}

uint32_t views()
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_TETHERING | DT_VIEW_DARKROOM | DT_VIEW_MAP;
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
  /* initialize base */
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_no_show_all(self->widget, TRUE);
  gtk_container_set_border_width(GTK_CONTAINER(self->widget), 5);

  /* setup proxy */
  dt_pthread_mutex_lock(&darktable.control->progress_system.mutex);

  darktable.control->progress_system.proxy.module = self;
  darktable.control->progress_system.proxy.added = _lib_backgroundjobs_added;
  darktable.control->progress_system.proxy.destroyed = _lib_backgroundjobs_destroyed;
  darktable.control->progress_system.proxy.cancellable = _lib_backgroundjobs_cancellable;
  darktable.control->progress_system.proxy.updated = _lib_backgroundjobs_updated;

  // iterate over darktable.control->progress_system.list and add everything that is already there and update
  // its gui_data!
  GList *iter = darktable.control->progress_system.list;
  while(iter)
  {
    dt_progress_t *progress = (dt_progress_t *)iter->data;
    void *gui_data = dt_control_progress_get_gui_data(progress);
    free(gui_data);
    gui_data = _lib_backgroundjobs_added(self, dt_control_progress_has_progress_bar(progress),
                                         dt_control_progress_get_message(progress));
    dt_control_progress_set_gui_data(progress, gui_data);
    if(dt_control_progress_cancellable(progress)) _lib_backgroundjobs_cancellable(self, gui_data, progress);
    _lib_backgroundjobs_updated(self, gui_data, dt_control_progress_get_progress(progress));
    iter = g_list_next(iter);
  }

  dt_pthread_mutex_unlock(&darktable.control->progress_system.mutex);
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* lets kill proxy */
  dt_pthread_mutex_lock(&darktable.control->progress_system.mutex);
  darktable.control->progress_system.proxy.module = NULL;
  darktable.control->progress_system.proxy.added = NULL;
  darktable.control->progress_system.proxy.destroyed = NULL;
  darktable.control->progress_system.proxy.cancellable = NULL;
  darktable.control->progress_system.proxy.updated = NULL;
  dt_pthread_mutex_unlock(&darktable.control->progress_system.mutex);
}

/** the proxy functions */

static void *_lib_backgroundjobs_added(dt_lib_module_t *self, gboolean has_progress_bar, const gchar *message)
{
  // add a new gui thingy
  dt_lib_backgroundjob_element_t *instance
      = (dt_lib_backgroundjob_element_t *)calloc(1, sizeof(dt_lib_backgroundjob_element_t));
  if(!instance) return NULL;

  /* lets make this threadsafe */
  gboolean i_own_lock = dt_control_gdk_lock();

  instance->widget = gtk_event_box_new();

  /* initialize the ui elements for job */
  gtk_widget_set_name(GTK_WIDGET(instance->widget), "background_job_eventbox");
  GtkBox *vbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  instance->hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 2);
  gtk_container_add(GTK_CONTAINER(instance->widget), GTK_WIDGET(vbox));

  /* add job label */
  GtkWidget *label = gtk_label_new(message);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(instance->hbox), GTK_WIDGET(label), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(instance->hbox), TRUE, TRUE, 0);

  /* use progressbar ? */
  if(has_progress_bar)
  {
    instance->progressbar = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(vbox), instance->progressbar, TRUE, FALSE, 2);
  }

  /* lets show jobbox if its hidden */
  gtk_box_pack_start(GTK_BOX(self->widget), instance->widget, TRUE, FALSE, 1);
  gtk_box_reorder_child(GTK_BOX(self->widget), instance->widget, 1);
  gtk_widget_show_all(instance->widget);
  gtk_widget_show(self->widget);

  if(i_own_lock) dt_control_gdk_unlock();

  // return the gui thingy container
  return instance;
}

static void _lib_backgroundjobs_destroyed(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance)
{
  // remove the gui that is pointed to in instance
  gboolean i_own_lock = dt_control_gdk_lock();

  /* remove job widget from jobbox */
  if(instance->widget && GTK_IS_WIDGET(instance->widget))
    gtk_container_remove(GTK_CONTAINER(self->widget), instance->widget);
  instance->widget = NULL;

  /* if jobbox is empty lets hide */
  if(g_list_length(gtk_container_get_children(GTK_CONTAINER(self->widget))) == 0)
    gtk_widget_hide(self->widget);

  if(i_own_lock) dt_control_gdk_unlock();

  // free data
  free(instance);
}

static void _lib_backgroundjobs_cancel_callback_new(GtkWidget *w, gpointer user_data)
{
  dt_progress_t *progress = (dt_progress_t *)user_data;
  dt_control_progress_cancel(darktable.control, progress);
}

static void _lib_backgroundjobs_cancellable(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance,
                                            dt_progress_t *progress)
{
  // add a cancel button to the gui. when clicked we want dt_control_progress_cancel(darktable.control,
  // progress); to be called
  if(!darktable.control->running) return;
  gboolean i_own_lock = dt_control_gdk_lock();

  GtkBox *hbox = GTK_BOX(instance->hbox);
  GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_cancel, CPF_STYLE_FLAT);
  gtk_widget_set_size_request(button, DT_PIXEL_APPLY_DPI(17), DT_PIXEL_APPLY_DPI(17));
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_lib_backgroundjobs_cancel_callback_new), progress);
  gtk_box_pack_start(hbox, GTK_WIDGET(button), FALSE, FALSE, 0);
  gtk_widget_show_all(button);

  if(i_own_lock) dt_control_gdk_unlock();
}

static void _lib_backgroundjobs_updated(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance,
                                        double value)
{
  // update the progress bar
  if(!darktable.control->running) return;
  gboolean i_own_lock = dt_control_gdk_lock();

  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(instance->progressbar), CLAMP(value, 0, 1.0));

  if(i_own_lock) dt_control_gdk_unlock();
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
