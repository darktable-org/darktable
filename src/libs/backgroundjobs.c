/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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
#include "common/image_cache.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/progress.h"
#include "develop/develop.h"
#include "dtgtk/button.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_backgroundjob_element_t
{
  GtkWidget *widget, *label, *progressbar, *hbox;
} dt_lib_backgroundjob_element_t;

/* proxy functions */
static void *_lib_backgroundjobs_added(dt_lib_module_t *self, gboolean has_progress_bar, const gchar *message);
static void _lib_backgroundjobs_destroyed(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance);
static void _lib_backgroundjobs_cancellable(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance,
                                            dt_progress_t *progress);
static void _lib_backgroundjobs_updated(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance,
                                        double value);
static void _lib_backgroundjobs_message_updated(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance,
                                                const gchar *message);


const char *name(dt_lib_module_t *self)
{
  return _("background jobs");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"*", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_BOTTOM;
}

int position()
{
  return 1;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize base */
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_no_show_all(self->widget, TRUE);

  /* setup proxy */
  dt_pthread_mutex_lock(&darktable.control->progress_system.mutex);

  darktable.control->progress_system.proxy.module = self;
  darktable.control->progress_system.proxy.added = _lib_backgroundjobs_added;
  darktable.control->progress_system.proxy.destroyed = _lib_backgroundjobs_destroyed;
  darktable.control->progress_system.proxy.cancellable = _lib_backgroundjobs_cancellable;
  darktable.control->progress_system.proxy.updated = _lib_backgroundjobs_updated;
  darktable.control->progress_system.proxy.message_updated = _lib_backgroundjobs_message_updated;

  // iterate over darktable.control->progress_system.list and add everything that is already there and update
  // its gui_data!
  for(const GList *iter = darktable.control->progress_system.list; iter; iter = g_list_next(iter))
  {
    dt_progress_t *progress = (dt_progress_t *)iter->data;
    void *gui_data = dt_control_progress_get_gui_data(progress);
    free(gui_data);
    gui_data = _lib_backgroundjobs_added(self, dt_control_progress_has_progress_bar(progress),
                                         dt_control_progress_get_message(progress));
    dt_control_progress_set_gui_data(progress, gui_data);
    if(dt_control_progress_cancellable(progress)) _lib_backgroundjobs_cancellable(self, gui_data, progress);
    _lib_backgroundjobs_updated(self, gui_data, dt_control_progress_get_progress(progress));
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

typedef struct _added_gui_thread_t
{
  GtkWidget *self_widget, *instance_widget;
} _added_gui_thread_t;

static gboolean _added_gui_thread(gpointer user_data)
{
  _added_gui_thread_t *params = (_added_gui_thread_t *)user_data;

  /* lets show jobbox if its hidden */
  gtk_box_pack_start(GTK_BOX(params->self_widget), params->instance_widget, TRUE, FALSE, 0);
  gtk_box_reorder_child(GTK_BOX(params->self_widget), params->instance_widget, 1);
  gtk_widget_show_all(params->instance_widget);
  gtk_widget_show(params->self_widget);

  free(params);
  return FALSE;
}

static void *_lib_backgroundjobs_added(dt_lib_module_t *self, gboolean has_progress_bar, const gchar *message)
{
  // add a new gui thingy
  dt_lib_backgroundjob_element_t *instance
      = (dt_lib_backgroundjob_element_t *)calloc(1, sizeof(dt_lib_backgroundjob_element_t));
  if(!instance) return NULL;
  _added_gui_thread_t *params = (_added_gui_thread_t *)malloc(sizeof(_added_gui_thread_t));
  if(!params)
  {
    free(instance);
    return NULL;
  }

  instance->widget = gtk_event_box_new();

  /* initialize the ui elements for job */
  gtk_widget_set_name(GTK_WIDGET(instance->widget), "background-job-eventbox");
  dt_gui_add_class(GTK_WIDGET(instance->widget), "dt_big_btn_canvas");
  GtkBox *vbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  instance->hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_container_add(GTK_CONTAINER(instance->widget), GTK_WIDGET(vbox));

  /* add job label */
  instance->label = gtk_label_new(message);
  gtk_widget_set_halign(instance->label, GTK_ALIGN_START);
  gtk_label_set_ellipsize(GTK_LABEL(instance->label), PANGO_ELLIPSIZE_END);
  gtk_box_pack_start(GTK_BOX(instance->hbox), GTK_WIDGET(instance->label), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(instance->hbox), TRUE, TRUE, 0);

  /* use progressbar ? */
  if(has_progress_bar)
  {
    instance->progressbar = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(vbox), instance->progressbar, TRUE, FALSE, 0);
  }

  /* lets show jobbox if its hidden */
  params->self_widget = self->widget;
  params->instance_widget = instance->widget;
  g_main_context_invoke(NULL, _added_gui_thread, params);

  // return the gui thingy container
  return instance;
}

typedef struct _destroyed_gui_thread_t
{
  dt_lib_module_t *self;
  dt_lib_backgroundjob_element_t *instance;
} _destroyed_gui_thread_t;

static gboolean _destroyed_gui_thread(gpointer user_data)
{
  _destroyed_gui_thread_t *params = (_destroyed_gui_thread_t *)user_data;

  /* remove job widget from jobbox */
  if(params->instance->widget && GTK_IS_WIDGET(params->instance->widget))
    gtk_container_remove(GTK_CONTAINER(params->self->widget), params->instance->widget);
  params->instance->widget = NULL;

  /* if jobbox is empty let's hide */
  if(!dt_gui_container_has_children(GTK_CONTAINER(params->self->widget)))
    gtk_widget_hide(params->self->widget);

  // free data
  free(params->instance);
  free(params);
  return FALSE;
}

// remove the gui that is pointed to in instance
static void _lib_backgroundjobs_destroyed(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance)
{
  _destroyed_gui_thread_t *params = (_destroyed_gui_thread_t *)malloc(sizeof(_destroyed_gui_thread_t));
  if(!params) return;
  params->self = self;
  params->instance = instance;
  g_main_context_invoke(NULL, _destroyed_gui_thread, params);
}

static void _lib_backgroundjobs_cancel_callback_new(GtkWidget *w, gpointer user_data)
{
  dt_progress_t *progress = (dt_progress_t *)user_data;
  dt_control_progress_cancel(darktable.control, progress);
}

typedef struct _cancellable_gui_thread_t
{
  dt_lib_backgroundjob_element_t *instance;
  dt_progress_t *progress;
} _cancellable_gui_thread_t;

static gboolean _cancellable_gui_thread(gpointer user_data)
{
  _cancellable_gui_thread_t *params = (_cancellable_gui_thread_t *)user_data;

  GtkBox *hbox = GTK_BOX(params->instance->hbox);
  GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_cancel, 0, NULL);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_lib_backgroundjobs_cancel_callback_new), params->progress);
  gtk_box_pack_start(hbox, GTK_WIDGET(button), FALSE, FALSE, 0);
  gtk_widget_show_all(button);

  free(params);
  return FALSE;
}

static void _lib_backgroundjobs_cancellable(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance,
                                            dt_progress_t *progress)
{
  // add a cancel button to the gui. when clicked we want dt_control_progress_cancel(darktable.control,
  // progress); to be called
  if(!darktable.control->running) return;

  _cancellable_gui_thread_t *params = (_cancellable_gui_thread_t *)malloc(sizeof(_cancellable_gui_thread_t));
  if(!params) return;
  params->instance = instance;
  params->progress = progress;
  g_main_context_invoke(NULL, _cancellable_gui_thread, params);
}

typedef struct _update_gui_thread_t
{
  dt_lib_backgroundjob_element_t *instance;
  double value;
} _update_gui_thread_t;

static gboolean _update_gui_thread(gpointer user_data)
{
  _update_gui_thread_t *params = (_update_gui_thread_t *)user_data;

  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(params->instance->progressbar), CLAMP(params->value, 0, 1.0));

  free(params);
  return FALSE;
}

static void _lib_backgroundjobs_updated(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance,
                                        double value)
{
  // update the progress bar
  if(!darktable.control->running) return;

  _update_gui_thread_t *params = (_update_gui_thread_t *)malloc(sizeof(_update_gui_thread_t));
  if(!params) return;
  params->instance = instance;
  params->value = value;
  g_main_context_invoke(NULL, _update_gui_thread, params);
}

typedef struct _update_label_gui_thread_t
{
  dt_lib_backgroundjob_element_t *instance;
  char *message;
} _update_label_gui_thread_t;

static gboolean _update_message_gui_thread(gpointer user_data)
{
  _update_label_gui_thread_t *params = (_update_label_gui_thread_t *)user_data;

  gtk_label_set_text(GTK_LABEL(params->instance->label), params->message);

  g_free(params->message);
  free(params);
  return FALSE;
}

static void _lib_backgroundjobs_message_updated(dt_lib_module_t *self, dt_lib_backgroundjob_element_t *instance,
                                                const char *message)
{
  // update the progress bar
  if(!darktable.control->running) return;

  _update_label_gui_thread_t *params = (_update_label_gui_thread_t *)malloc(sizeof(_update_label_gui_thread_t));
  if(!params) return;
  params->instance = instance;
  params->message = g_strdup(message);
  g_main_context_invoke(NULL, _update_message_gui_thread, params);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

