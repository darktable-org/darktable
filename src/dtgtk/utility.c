/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers.

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

#include "dtgtk/utility.h"

gboolean dtgtk_container_has_children(GtkContainer *container)
{
  g_return_val_if_fail(GTK_IS_CONTAINER(container), FALSE);
  GList *children = gtk_container_get_children(container);
  gboolean has_children = children != NULL;
  g_list_free(children);
  return has_children;
}

int dtgtk_container_num_children(GtkContainer *container)
{
  g_return_val_if_fail(GTK_IS_CONTAINER(container), FALSE);
  GList *children = gtk_container_get_children(container);
  int num_children = g_list_length(children);
  g_list_free(children);
  return num_children;
}

GtkWidget *dtgtk_container_first_child(GtkContainer *container)
{
  g_return_if_fail(GTK_IS_CONTAINER(container));
  GList *children = gtk_container_get_children(container);
  GtkWidget *child = children ? (GtkWidget*)children->data : NULL;
  g_list_free(children);
  return child;
}

GtkWidget *dtgtk_container_nth_child(GtkContainer *container, int which)
{
  g_return_if_fail(GTK_IS_CONTAINER(container));
  GList *children = gtk_container_get_children(container);
  GtkWidget *child = (GtkWidget*)g_list_nth_data(children, which);
  g_list_free(children);
  return child;
}

static void _remove_child(GtkWidget *widget, gpointer data)
{
  gtk_container_remove((GtkContainer*)data, widget);
}

void dtgtk_container_remove_children(GtkContainer *container)
{
  g_return_if_fail(GTK_IS_CONTAINER(container));
  gtk_container_foreach(container, _remove_child, container);
}

static void _delete_child(GtkWidget *widget, gpointer data)
{
  (void)data;  // avoid unreferenced-parameter warning
  gtk_widget_destroy(widget);
}

void dtgtk_container_destroy_children(GtkContainer *container)
{
  g_return_if_fail(GTK_IS_CONTAINER(container));
  gtk_container_foreach(container, _delete_child, NULL);
}
