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
#ifndef DT_GUI_IOP_HISTORY_H
#define DT_GUI_IOP_HISTORY_H

#include <inttypes.h>
#include <gtk/gtk.h>
#include <glib.h>

void dt_gui_iop_history_init ();

/** resets ui history */
void dt_gui_iop_history_reset ();
/** add history item to ui */
GtkWidget * dt_gui_iop_history_add_item (uint32_t num, const gchar *label);
uint32_t dt_gui_iop_history_get_top();
/** removes item on top */
void dt_gui_iop_history_pop_top();
#endif