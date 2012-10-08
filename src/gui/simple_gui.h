/*
		This file is part of darktable,
		copyright (c) 2012 tobias ellinghaus.

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

#ifndef DT_GUI_SIMPLE_API_H
#define DT_GUI_SIMPLE_API_H

#include "dtgtk/paint.h"

/** possible types of gui elements */
typedef enum dt_gui_simple_type_t
{
  DT_SIMPLE_GUI_NONE,
  DT_SIMPLE_GUI_SLIDER,
  DT_SIMPLE_GUI_COMBOBOX,
  DT_SIMPLE_GUI_BUTTON,
  DT_SIMPLE_GUI_TOGGLE_BUTTON
} dt_gui_simple_type_t;

// easy access to common fields of widgets
typedef struct dt_gui_simple_common_t
{
  dt_gui_simple_type_t type;
  char *id;
  char *label;
  char *tooltip;
} dt_gui_simple_common_t;

typedef struct dt_gui_simple_slider_t
{
  dt_gui_simple_type_t type;             // DT_SIMPLE_GUI_SLIDER
  char *id;
  char *label;
  char *tooltip;
  char *format;
  float min, max, step, defval;
  int digits;
  void (*value_changed)(GtkWidget *w, gpointer data);
  gpointer parameter;
} dt_gui_simple_slider_t;

typedef struct dt_gui_simple_combobox_t
{
  dt_gui_simple_type_t type;             // DT_SIMPLE_GUI_COMBOBOX
  char *id;
  char *label;
  char *tooltip;
  char **entries;                        // always terminate with NULL
  int defval;
  void (*value_changed)(GtkWidget *w, gpointer data);
  void *parameter;
} dt_gui_simple_combobox_t;

// used both for buttons and toggle buttons
typedef struct dt_gui_simple_button_t
{
  dt_gui_simple_type_t type;             // DT_SIMPLE_GUI_BUTTON or DT_SIMPLE_GUI_TOGGLE_BUTTON
  char *id;
  char *label;
  char *tooltip;
  DTGTKCairoPaintIconFunc paint;
  int paintflags;
  int defval;
  void (*clicked)(GtkWidget *w, gpointer data);
  void *parameter;
} dt_gui_simple_button_t;

/** a single element of the gui, access union according to type */
typedef union dt_gui_simple_element_t
{
  dt_gui_simple_common_t common;
  dt_gui_simple_slider_t slider;
  dt_gui_simple_combobox_t combobox;
  dt_gui_simple_button_t button;
} dt_gui_simple_element_t;

/** the data type returned */
typedef struct dt_gui_simple_t
{
  int flags;                             // currently not used
  dt_gui_simple_element_t elements[];    // array of gui elements, terminated by an element of type DT_SIMPLE_GUI_NONE
} dt_gui_simple_t;

#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
