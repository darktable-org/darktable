/*
    This file is part of darktable,
    Copyright (C) 2015-2021 darktable developers.

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

#include "dtgtk/expander.h"

#include <gtk/gtk.h>

G_DEFINE_TYPE(GtkDarktableExpander, dtgtk_expander, GTK_TYPE_BOX);

static void dtgtk_expander_class_init(GtkDarktableExpanderClass *class)
{
}

GtkWidget *dtgtk_expander_get_frame(GtkDarktableExpander *expander)
{
  g_return_val_if_fail(DTGTK_IS_EXPANDER(expander), NULL);

  return expander->frame;
}

GtkWidget *dtgtk_expander_get_header(GtkDarktableExpander *expander)
{
  g_return_val_if_fail(DTGTK_IS_EXPANDER(expander), NULL);

  return expander->header;
}

GtkWidget *dtgtk_expander_get_header_event_box(GtkDarktableExpander *expander)
{
  g_return_val_if_fail(DTGTK_IS_EXPANDER(expander), NULL);

  return expander->header_evb;
}

GtkWidget *dtgtk_expander_get_body(GtkDarktableExpander *expander)
{
  g_return_val_if_fail(DTGTK_IS_EXPANDER(expander), NULL);

  return expander->body;
}

GtkWidget *dtgtk_expander_get_body_event_box(GtkDarktableExpander *expander)
{
  g_return_val_if_fail(DTGTK_IS_EXPANDER(expander), NULL);

  return expander->body_evb;
}

void dtgtk_expander_set_expanded(GtkDarktableExpander *expander, gboolean expanded)
{
  g_return_if_fail(DTGTK_IS_EXPANDER(expander));

  expanded = expanded != FALSE;

  if(expander->expanded != expanded)
  {
    expander->expanded = expanded;

    GtkWidget *frame = expander->body;

    if(frame)
    {
      gtk_widget_set_visible(frame, expander->expanded);
    }
  }
}

gboolean dtgtk_expander_get_expanded(GtkDarktableExpander *expander)
{
  g_return_val_if_fail(DTGTK_IS_EXPANDER(expander), FALSE);

  return expander->expanded;
}

static void dtgtk_expander_init(GtkDarktableExpander *expander)
{
}

// public functions
GtkWidget *dtgtk_expander_new(GtkWidget *header, GtkWidget *body)
{
  GtkDarktableExpander *expander;

  g_return_val_if_fail(GTK_IS_WIDGET(header), NULL);
  g_return_val_if_fail(GTK_IS_WIDGET(body), NULL);

  expander
      = g_object_new(dtgtk_expander_get_type(), "orientation", GTK_ORIENTATION_VERTICAL, "spacing", 0, NULL);
  expander->expanded = -1;
  expander->header = header;
  expander->body = body;

  expander->header_evb = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(expander->header_evb), expander->header);
  expander->body_evb = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(expander->body_evb), expander->body);
  expander->frame = gtk_frame_new(NULL);
  gtk_container_add(GTK_CONTAINER(expander->frame), expander->body_evb);

  gtk_box_pack_start(GTK_BOX(expander), expander->header_evb, TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(expander), expander->frame, TRUE, FALSE, 0);

  return GTK_WIDGET(expander);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

