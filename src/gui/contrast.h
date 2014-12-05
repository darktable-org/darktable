/*
    This file is part of darktable,
    copyright (c) 2009--2011 Henrik Andersson.

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
#ifndef DT_GUI_CONTRAST_H
#define DT_GUI_CONTRAST_H

#include <inttypes.h>
#include <gtk/gtk.h>
#include <glib.h>

/** initializes the userinterface contrast */
void dt_gui_contrast_init();
/** increases the contrast */
void dt_gui_contrast_increase();
/** decreases the contrast */
void dt_gui_contrast_decrease();
/** increase brightness */
void dt_gui_brightness_increase();
/** decrease brightness */
void dt_gui_brightness_decrease();
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
