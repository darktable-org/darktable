/*
    This file is part of darktable,
    copyright (c) 2012 johannes hanika.

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

#ifndef DT_BAUHAUS_H
#define DT_BAUHAUS_H

#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "common/debug.h"
#include "gui/gtk.h"
#include "gui/draw.h"

#include <sys/select.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <gdk/gdkkeysyms.h>

#define DT_BAUHAUS_WIDGET_TYPE             (dt_bh_get_type ())
#define DT_BAUHAUS_WIDGET(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), DT_BAUHAUS_WIDGET_TYPE, DtBauhausWidget))
#define DT_BAUHAUS_WIDGET_CLASS(obj)       (G_TYPE_CHECK_CLASS_CAST ((obj), DT_BAUHAUS_WIDGET, DtBauhausWidgetClass))
#define DT_IS_BAUHAUS_WIDGET(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), DT_BAUHAUS_WIDGET_TYPE))
#define DT_IS_BAUHAUS_WIDGET_CLASS(obj)    (G_TYPE_CHECK_CLASS_TYPE ((obj), DT_BAUHAUS_WIDGET_TYPE))
#define DT_BAUHAUS_WIDGET_GET_CLASS        (G_TYPE_INSTANCE_GET_CLASS ((obj), DT_BAUHAUS_WIDGET_TYPE, DtBauausWidgetClass))


typedef enum dt_bauhaus_type_t
{
  DT_BAUHAUS_SLIDER = 1,
  DT_BAUHAUS_COMBOBOX = 2,
}
dt_bauhaus_type_t;

// data portion for a slider
typedef struct dt_bauhaus_slider_data_t
{
  float pos;
  float scale;
  char format[8];
}
dt_bauhaus_slider_data_t;

// data portion for a combobox
typedef struct dt_bauhaus_combobox_data_t
{
  // list of strings, probably
  // TODO:
  // could be one char ** and dynamic malloc.
}
dt_bauhaus_combobox_data_t;

typedef union dt_bauhaus_data_t
{
  // this is the placeholder for the data portions
  // associated with the implementations such as
  // siders, combo boxes, ..
  dt_bauhaus_slider_data_t slider;
  dt_bauhaus_combobox_data_t combobox;
}
dt_bauhaus_data_t;

// gah, caps.
typedef struct dt_bauhaus_widget_t DtBauhausWidget;
typedef struct dt_bauhaus_widget_class_t DtBauhausWidgetClass;

// our new widget and its private members, inheriting from drawing area:
typedef struct dt_bauhaus_widget_t
{
  GtkDrawingArea parent;
  // which type of control
  dt_bauhaus_type_t type;
  // associated drawing area in gtk
  GtkWidget *area;
  // associated image operation module (to handle focus and such)
  dt_iop_module_t *module;
  // TODO: callbacks for user signals?

  // goes last, might extend past the end:
  dt_bauhaus_data_t data;
}
dt_bauhaus_widget_t;

// class of our new widget, inheriting from drawing area
typedef struct dt_bauhaus_widget_class_t
{
  GtkDrawingAreaClass parent_class;
}
dt_bauhaus_widget_class_t;

// global static data:
enum
{
  DT_BAUHAUS_VALUE_CHANGED_SIGNAL,
  DT_BAUHAUS_LAST_SIGNAL
};

typedef struct dt_bauhaus_t
{
  dt_bauhaus_widget_t *current;
  GtkWidget *popup_window;
  GtkWidget *popup_area;
  // are set by the motion notification, to be used during drawing.
  float mouse_x, mouse_y;
  // pointer position when popup window is closed
  float end_mouse_x, end_mouse_y;
  // key input buffer
  char keys[64];
  int keys_cnt;
  // our custom signals
  guint signals[DT_BAUHAUS_LAST_SIGNAL];
}
dt_bauhaus_t;

// FIXME: into darktable.h
dt_bauhaus_t bauhaus;


void dt_bauhaus_init();
void dt_bauhaus_cleanup();

GtkWidget* dt_bauhaus_slider_new(dt_iop_module_t *self);
GtkWidget* dt_bauhaus_combobox_new(dt_iop_module_t *self);

#endif
