/*
    This file is part of darktable,
    copyright (c) 2010 tobias ellinghaus.

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

#include <gtk/gtk.h>
#include <glade/glade.h>

#include "common/darktable.h"
#include "common/colorlabels.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"

static void
color_label_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_colorlabels_key_accel_callback(user_data);
  dt_control_queue_draw_all();
}

void dt_create_color_label_buttons(GtkBox *toolbox){
	GtkBox *hbox;
	GtkWidget *button;
	hbox = GTK_BOX(gtk_hbox_new(FALSE, 2));
	gtk_container_set_border_width (GTK_CONTAINER (hbox),2);
	button = dtgtk_button_new(dtgtk_cairo_paint_label, (0|8));
	gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("toggle red label\nof selected images (f1)"), (char *)NULL);
	gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
	g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(color_label_button_clicked), (gpointer)0);

	button = dtgtk_button_new(dtgtk_cairo_paint_label, (1|8));
	gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("toggle yellow label\nof selected images (f2)"), (char *)NULL);
	gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
	g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(color_label_button_clicked), (gpointer)1);

	button = dtgtk_button_new(dtgtk_cairo_paint_label, (2|8));
	gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("toggle green label\nof selected images (f3)"), (char *)NULL);
	gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
	g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(color_label_button_clicked), (gpointer)2);

	button = dtgtk_button_new(dtgtk_cairo_paint_label, (3|8));
	gtk_object_set(GTK_OBJECT(button), "tooltip-text", _("clear all labels of selected images"), (char *)NULL);
	gtk_box_pack_start(hbox, button, TRUE, TRUE, 0);
	g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(color_label_button_clicked), (gpointer)3);

	gtk_box_pack_start(toolbox,GTK_WIDGET(hbox),FALSE,FALSE,0);
	gtk_widget_show_all (GTK_WIDGET (toolbox));
}
