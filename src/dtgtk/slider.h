/*
    This file is part of darktable,
    copyright (c) 2010 Henrik Andersson.

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
#ifndef DTGTK_SLIDER_H
#define DTGTK_SLIDER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS
#define DTGTK_SLIDER(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, dtgtk_slider_get_type(), GtkDarktableSlider)
#define DTGTK_SLIDER_CLASS(klass)                                                                            \
  GTK_CHECK_CLASS_CAST(klass, dtgtk_slider_get_type(), GtkDarktableSliderClass)
#define DTGTK_IS_SLIDER(obj) G_TYPE_CHECK_INSTANCE_TYPE(obj, dtgtk_slider_get_type())
#define DTGTK_IS_SLIDER_CLASS(klass) GTK_CHECK_CLASS_TYPE(obj, dtgtk_slider_get_type())

typedef enum _darktable_slider_type
{
  /** Default type , a standard slider, this is default */
  DARKTABLE_SLIDER_BAR = 0,
  /** Value slider, doesn't show a bar, just a value */
  DARKTABLE_SLIDER_VALUE
} darktable_slider_type_t;

typedef enum _darktable_slider_format_type
{
  /** Default format type , value is displayed as float*/
  DARKTABLE_SLIDER_FORMAT_FLOAT = 0,
  /** Value is displayed as ratio  eg. 50/50*/
  DARKTABLE_SLIDER_FORMAT_RATIO,
  DARKTABLE_SLIDER_FORMAT_PERCENT,
  DARKTABLE_SLIDER_FORMAT_NONE
} darktable_slider_format_type_t;

typedef struct _GtkDarktableSlider
{
  GtkEventBox widget;
  GtkWidget *entry;
  GtkHBox *hbox;
  GtkAdjustment *adjustment;
  gboolean is_dragging;
  gboolean is_sensibility_key_pressed;
  gboolean is_entry_active;
  gboolean is_changed;
  gboolean force_sign;
  gint prev_x_root;
  gint motion_direction;
  gint digits;
  gint snapsize;
  gint labelwidth;
  gint labelheight;
  gdouble default_value;
  darktable_slider_type_t type;
  darktable_slider_format_type_t fmt_type;
} GtkDarktableSlider;


typedef struct _GtkDarktableSliderClass
{
  GtkEventBoxClass parent_class;
} GtkDarktableSliderClass;

enum
{
  VALUE_CHANGED,
  SLIDER_LAST_SIGNAL
};

GType dtgtk_slider_get_type(void);

/** Instansiate a new darktable slider control passing adjustment as range */
GtkWidget *dtgtk_slider_new(GtkAdjustment *adjustment);
/** Instansiate a new slider with specified range values */
GtkWidget *dtgtk_slider_new_with_range(darktable_slider_type_t type, gdouble min, gdouble max, gdouble step,
                                       gdouble value, gint digits);
/** Get the value of the slider */
gdouble dtgtk_slider_get_value(GtkDarktableSlider *slider);
/** Set label of slider */
void dtgtk_slider_set_label(GtkDarktableSlider *slider, gchar *label);
/** Set unit of value */
void dtgtk_slider_set_unit(GtkDarktableSlider *slider, gchar *unit);
/** Set the default value of slider */
void dtgtk_slider_set_default_value(GtkDarktableSlider *slider, gdouble val);
/** Set force of sign of positive in displayed value */
void dtgtk_slider_set_force_sign(GtkDarktableSlider *slider, gboolean force);
/** Set the value of the slider */
void dtgtk_slider_set_value(GtkDarktableSlider *slider, gdouble value);
/** Set the type of the slider */
void dtgtk_slider_set_type(GtkDarktableSlider *slider, darktable_slider_type_t type);
/** Set digits to use when displaying value*/
void dtgtk_slider_set_digits(GtkDarktableSlider *slider, gint digits);
/** Set value display format */
void dtgtk_slider_set_format_type(GtkDarktableSlider *slider, darktable_slider_format_type_t type);
/** set step size to snap values to. 0 for no snapping (default). */
void dtgtk_slider_set_snap(GtkDarktableSlider *slider, gint snapsize);

G_END_DECLS
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
