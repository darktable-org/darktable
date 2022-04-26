/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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
#include "develop/develop.h"
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include "gui/accelerators.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

enum
{
  TEXT_COLUMN,
  VIEW_COLUMN,
  SENSITIVE_COLUMN,
  N_COLUMNS
};

typedef struct dt_lib_viewswitcher_t
{
  GList *labels;
  GtkWidget *dropdown;
} dt_lib_viewswitcher_t;

/* callback when a view label is pressed */
static gboolean _lib_viewswitcher_button_press_callback(GtkWidget *w, GdkEventButton *ev, gpointer user_data);
/* helper function to create a label */
static GtkWidget *_lib_viewswitcher_create_label(dt_view_t *view);
/* callback when view changed signal happens */
static void _lib_viewswitcher_view_changed_callback(gpointer instance, dt_view_t *old_view,
                                                    dt_view_t *new_view, gpointer user_data);
static void _lib_viewswitcher_view_cannot_change_callback(gpointer instance, dt_view_t *old_view,
                                                          dt_view_t *new_view, gpointer user_data);
static void _switch_view(const dt_view_t *view);

const char *name(dt_lib_module_t *self)
{
  return _("viewswitcher");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"*", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_TOP_RIGHT;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1001;
}

static void _dropdown_changed(GtkComboBox *widget, gpointer user_data)
{
  dt_lib_viewswitcher_t *d = (dt_lib_viewswitcher_t *)user_data;

  GtkTreeIter iter;
  if(gtk_combo_box_get_active_iter(GTK_COMBO_BOX(d->dropdown), &iter))
  {
    const dt_view_t *view;
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(d->dropdown));
    gtk_tree_model_get(model, &iter, VIEW_COLUMN, &view, -1);
    _switch_view(view);
  }
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_viewswitcher_t *d = (dt_lib_viewswitcher_t *)g_malloc0(sizeof(dt_lib_viewswitcher_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  d->dropdown = NULL;
  GtkTreeIter tree_iter;
  GtkListStore *model = NULL;

  for(GList *view_iter = darktable.view_manager->views; view_iter; view_iter = g_list_next(view_iter))
  {
    dt_view_t *view = (dt_view_t *)view_iter->data;
    // lighttable and darkroom are shown in the top level, the rest in a dropdown
    /* create view label */

    // skip hidden views
    if(view->flags() & VIEW_FLAGS_HIDDEN) continue;

    if(!g_strcmp0(view->module_name, "lighttable") || !g_strcmp0(view->module_name, "darkroom"))
    {
      GtkWidget *w = _lib_viewswitcher_create_label(view);
      gtk_box_pack_start(GTK_BOX(self->widget), w, FALSE, FALSE, 0);
      d->labels = g_list_append(d->labels, gtk_bin_get_child(GTK_BIN(w)));

      dt_action_define(&darktable.control->actions_global, "switch views", view->module_name, w, NULL);

      /* create space if more views */
      if(view_iter->next != NULL)
      {
        GtkWidget *sep = gtk_label_new("|");
        gtk_widget_set_halign(sep, GTK_ALIGN_START);
        gtk_widget_set_name(sep, "view-label");
        gtk_box_pack_start(GTK_BOX(self->widget), sep, FALSE, FALSE, 0);
      }
    }
    else
    {
      // only create the dropdown when needed, in case someone runs dt with just lt + dr
      if(!d->dropdown)
      {
        model = gtk_list_store_new(N_COLUMNS, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_BOOLEAN);
        d->dropdown = gtk_combo_box_new_with_model(GTK_TREE_MODEL(model));
        gtk_widget_set_name(d->dropdown, "view-dropdown");
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(d->dropdown), renderer, FALSE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(d->dropdown), renderer, "markup", TEXT_COLUMN,
                                        "sensitive", SENSITIVE_COLUMN, NULL);

        gtk_list_store_append(model, &tree_iter);
        gtk_list_store_set(model, &tree_iter, TEXT_COLUMN, /*italic*/ _("other"), VIEW_COLUMN, NULL, SENSITIVE_COLUMN, 0, -1);

        gtk_box_pack_start(GTK_BOX(self->widget), d->dropdown, FALSE, FALSE, 0);
        g_signal_connect(G_OBJECT(d->dropdown), "changed", G_CALLBACK(_dropdown_changed), d);
      }

      gtk_list_store_append(model, &tree_iter);
      gtk_list_store_set(model, &tree_iter, TEXT_COLUMN, view->name(view), VIEW_COLUMN, view, SENSITIVE_COLUMN, 1, -1);
    }
  }

  if(model) g_object_unref(model);

  /* connect callback to view change signal */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,
                            G_CALLBACK(_lib_viewswitcher_view_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CANNOT_CHANGE,
                                  G_CALLBACK(_lib_viewswitcher_view_cannot_change_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_viewswitcher_view_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_viewswitcher_view_cannot_change_callback), self);
  g_free(self->data);
  self->data = NULL;
}

static void _lib_viewswitcher_enter_leave_notify_callback(GtkWidget *w, GdkEventCrossing *e, gpointer user_data)
{
  GtkLabel *l = (GtkLabel *)user_data;

  /* if not active view lets highlight */
  if(e->type == GDK_ENTER_NOTIFY &&
     strcmp(g_object_get_data(G_OBJECT(w), "view-label"), dt_view_manager_name(darktable.view_manager)))
    gtk_widget_set_state_flags(GTK_WIDGET(l), GTK_STATE_FLAG_PRELIGHT, FALSE);
  else
    gtk_widget_unset_state_flags(GTK_WIDGET(l), GTK_STATE_FLAG_PRELIGHT);
}

static void _lib_viewswitcher_view_cannot_change_callback(gpointer instance, dt_view_t *old_view,
                                                          dt_view_t *new_view, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_viewswitcher_t *d = (dt_lib_viewswitcher_t *)self->data;

  g_signal_handlers_block_by_func(d->dropdown, _dropdown_changed, d);
  gtk_combo_box_set_active(GTK_COMBO_BOX(d->dropdown), 0);
  gtk_widget_set_state_flags(d->dropdown, GTK_STATE_FLAG_SELECTED, FALSE);
  g_signal_handlers_unblock_by_func(d->dropdown, _dropdown_changed, d);
}

static void _lib_viewswitcher_view_changed_callback(gpointer instance, dt_view_t *old_view,
                                                    dt_view_t *new_view, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_viewswitcher_t *d = (dt_lib_viewswitcher_t *)self->data;

  const char *name = dt_view_manager_name(darktable.view_manager);
  gboolean found = FALSE;

  for(GList *iter = d->labels; iter; iter = g_list_next(iter))
  {
    GtkWidget *label = GTK_WIDGET(iter->data);
    if(!g_strcmp0(g_object_get_data(G_OBJECT(label), "view-label"), name))
    {
      gtk_widget_set_state_flags(label, GTK_STATE_FLAG_SELECTED, TRUE);
      found = TRUE;
    }
    else
      gtk_widget_set_state_flags(label, GTK_STATE_FLAG_NORMAL, TRUE);
  }

  g_signal_handlers_block_by_func(d->dropdown, _dropdown_changed, d);

  if(found)
  {
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->dropdown), 0);
    gtk_widget_set_state_flags(d->dropdown, GTK_STATE_FLAG_NORMAL, TRUE);
  }
  else
  {
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(d->dropdown));
    GtkTreeIter iter;
    uint32_t index = 0;
    if(gtk_tree_model_get_iter_first(model, &iter) == TRUE) do
    {
      gchar *str;
      gtk_tree_model_get(model, &iter, TEXT_COLUMN, &str, -1);
      if(!g_strcmp0(str, name))
      {
        gtk_combo_box_set_active(GTK_COMBO_BOX(d->dropdown), index);
        gtk_widget_set_state_flags(d->dropdown, GTK_STATE_FLAG_SELECTED, TRUE);
        break;
      }
      g_free(str);
      index++;
    } while(gtk_tree_model_iter_next(model, &iter) == TRUE);
  }

  g_signal_handlers_unblock_by_func(d->dropdown, _dropdown_changed, d);
}

static GtkWidget *_lib_viewswitcher_create_label(dt_view_t *view)
{
  GtkWidget *eb = gtk_event_box_new();
  GtkWidget *b = gtk_label_new(view->name(view));
  gtk_container_add(GTK_CONTAINER(eb), b);
  /*setup label*/
  gtk_widget_set_halign(b, GTK_ALIGN_START);
  g_object_set_data(G_OBJECT(b), "view-label", (gchar *)view->name(view));
  g_object_set_data(G_OBJECT(eb), "view-label", (gchar *)view->name(view));
  gtk_widget_set_name(b, "view-label");
  gtk_widget_set_state_flags(b, GTK_STATE_FLAG_NORMAL, TRUE);

  /* connect button press handler */
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_lib_viewswitcher_button_press_callback), view);

  /* set enter/leave notify events and connect signals */
  gtk_widget_add_events(GTK_WIDGET(eb), GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);

  g_signal_connect(G_OBJECT(eb), "enter-notify-event", G_CALLBACK(_lib_viewswitcher_enter_leave_notify_callback), b);
  g_signal_connect(G_OBJECT(eb), "leave-notify-event", G_CALLBACK(_lib_viewswitcher_enter_leave_notify_callback), b);

  return eb;
}

static void _switch_view(const dt_view_t *view)
{
  dt_ctl_switch_mode_to_by_view(view);
}

static gboolean _lib_viewswitcher_button_press_callback(GtkWidget *w, GdkEventButton *ev, gpointer user_data)
{
  if(ev->button == 1)
  {
    const dt_view_t *view = (const dt_view_t *)user_data;
    _switch_view(view);
    return TRUE;
  }
  return FALSE;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

