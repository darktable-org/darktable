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
#ifndef DT_GUI_PANEL_SIZEGROUP_H
#define DT_GUI_PANEL_SIZEGROUP_H

#include <gtk/gtk.h>

/** initialize the panel sizegroup */
void dt_gui_panel_sizegroup_init ();

/** add widget to panel sizegroup */
void dt_gui_panel_sizegroup_add (GtkWidget *w);

/** remove widget  frompanel sizegroup */
void dt_gui_panel_sizegroup_remove (GtkWidget *w);

/** runs thru modules to get each width allocation */
void dt_gui_panel_sizegroup_modules ();

#endif