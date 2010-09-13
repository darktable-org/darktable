/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include "common/colorlabels.h"
#include "control/control.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

const char*
name ()
{
  return _("color labels");
}

uint32_t views() 
{
  return DT_LIGHTTABLE_VIEW;
}

static void
button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_colorlabels_key_accel_callback(user_data);
  dt_control_queue_draw_all();
}

void
gui_reset (dt_lib_module_t *self)
{
  // maybe we want to maintain these for a while longer:
  // sqlite3_exec(darktable.db, "delete from color_labels", NULL, NULL, NULL);
}

int
position ()
{
  return 850;
}

/* drawing code taken from views/view.c to be consistent with actual labels*/
static void
on_expose_event(GtkWidget *widget, GdkEventExpose *event, gpointer data)
{
  const long int mode = (long int)data;

  cairo_t *cr = gdk_cairo_create (widget->window);

  GtkStyle *style=gtk_widget_get_style(widget);
  int state = gtk_widget_get_state(widget);

  // FIXME: buttons seem to have a gradient :-S
  if(state != GTK_STATE_NORMAL)
  {
    cairo_set_source_rgb (cr, style->bg[state].red/65535.0, style->bg[state].green/65535.0, style->bg[state].blue/65535.0);
    cairo_paint(cr);
  }

  switch(mode)
  {
    case 0: cairo_set_source_rgb(cr, 0.8, 0.2, 0.2); break;
    case 1: cairo_set_source_rgb(cr, 0.8, 0.8, 0.2); break;
    case 2: cairo_set_source_rgb(cr, 0.2, 0.8, 0.2); break;
    default: return;
  }

  cairo_translate(cr, 10, 10);
  cairo_arc(cr, 0.0, 0.0, 9, 0.0, 2.0*M_PI);
  cairo_fill_preserve(cr);
  cairo_set_line_width(cr, 2.0);
  cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
  cairo_stroke(cr);

  cairo_destroy(cr);
}

void
gui_init (dt_lib_module_t *self)
{
  self->data = NULL;
  self->widget = gtk_vbox_new(TRUE, 5);
  
  GtkBox *hbox;
  GtkWidget *button;
  GtkWidget *drawing_area;
  hbox = GTK_BOX(gtk_hbox_new(FALSE, 5));

  button = gtk_button_new();
  drawing_area = gtk_drawing_area_new();
  gtk_widget_set_size_request (drawing_area, 20, 20);
  g_signal_connect(drawing_area, "expose-event", G_CALLBACK (on_expose_event), (gpointer)0);
  gtk_button_set_image((GtkButton*)button, drawing_area);
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("toggle red label\nof selected images (ctrl-1)"), NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)0);

  button = gtk_button_new();
  drawing_area = gtk_drawing_area_new();
  gtk_widget_set_size_request (drawing_area, 20, 20);
  g_signal_connect(drawing_area, "expose-event", G_CALLBACK (on_expose_event), (gpointer)1);
  gtk_button_set_image((GtkButton*)button, drawing_area);
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("toggle yellow label\nof selected images (ctrl-2)"), NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)1);

  button = gtk_button_new();
  drawing_area = gtk_drawing_area_new();
  gtk_widget_set_size_request (drawing_area, 20, 20);
  g_signal_connect(drawing_area, "expose-event", G_CALLBACK (on_expose_event), (gpointer)2);
  gtk_button_set_image((GtkButton*)button, drawing_area);
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("toggle green label\nof selected images (ctrl-3)"), NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)2);

  button = gtk_button_new_with_label(_("clear"));
  gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("clear all labels of selected images"), NULL);
  gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), (gpointer)3);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
  dt_colorlabels_register_key_accels();
}

void
gui_cleanup (dt_lib_module_t *self)
{
  dt_colorlabels_unregister_key_accels();
}

