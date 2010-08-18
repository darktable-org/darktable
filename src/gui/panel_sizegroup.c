/*
    This file is part of darktable,
    copyright (c) 2009--2010 Henrik Andersson.

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

#include <glade/glade.h>
#include  "common/darktable.h"
#include "gui/gtk.h"
#include "gui/panel_sizegroup.h"

GtkSizeGroup *_panel_sizegroup = NULL;

void 
dt_gui_panel_sizegroup_init()
{
 // GtkWidget *widget=NULL;
  
  _panel_sizegroup = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
  
  /* dont ignore hidden widgets */
  gtk_size_group_set_ignore_hidden (_panel_sizegroup,FALSE);
  
  /* add all panels that exists in glade file */
  
  /* left panel */
/*  widget = glade_xml_get_widget (darktable.gui->main_window, "darktable_label_eventbox");
  dt_gui_panel_sizegroup_add (widget);
  widget = glade_xml_get_widget (darktable.gui->main_window, "navigation_expander");
  dt_gui_panel_sizegroup_add (widget);
  widget = glade_xml_get_widget (darktable.gui->main_window, "devices_expander");
  dt_gui_panel_sizegroup_add (widget);
  widget = glade_xml_get_widget (darktable.gui->main_window, "library_expander");
  dt_gui_panel_sizegroup_add (widget);
  widget = glade_xml_get_widget (darktable.gui->main_window, "metadata_expander");
  dt_gui_panel_sizegroup_add (widget);
  widget = glade_xml_get_widget (darktable.gui->main_window, "snapshots_expander");
  dt_gui_panel_sizegroup_add (widget);
  widget = glade_xml_get_widget (darktable.gui->main_window, "history_expander");
  dt_gui_panel_sizegroup_add (widget);*/
  
  
  /* right panel */
  /*widget = glade_xml_get_widget (darktable.gui->main_window, "histogram_expander");
  dt_gui_panel_sizegroup_add (widget);
  widget = glade_xml_get_widget (darktable.gui->main_window, "module_list_expander");
  dt_gui_panel_sizegroup_add (widget);  */
 
}

void 
dt_gui_panel_sizegroup_add (GtkWidget *w) 
{
  gtk_size_group_add_widget (_panel_sizegroup,w);
}

void 
dt_gui_panel_sizegroup_remove (GtkWidget *w)
{
  gtk_size_group_remove_widget (_panel_sizegroup,w);
}