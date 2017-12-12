/*
    This file is part of darktable,
    copyright (c) 2010--2012 Henrik Andersson.
    copyright (c) 2012 Ulrich Pegelow.

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

#pragma once

#define GRADIENT_SLIDER_MAX_POSITIONS 10

#include "paint.h"
#include <gtk/gtk.h>
G_BEGIN_DECLS
#define DTGTK_GRADIENT_SLIDER(obj)                                                                           \
  G_TYPE_CHECK_INSTANCE_CAST(obj, dtgtk_gradient_slider_get_type(), GtkDarktableGradientSlider)
#define DTGTK_GRADIENT_SLIDER_CLASS(klass)                                                                   \
  G_TYPE_CHECK_CLASS_CAST(klass, dtgtk_gradient_slider_get_type(), GtkDarktableGradientSliderClass)
#define DTGTK_IS_GRADIENT_SLIDER(obj) G_TYPE_CHECK_INSTANCE_TYPE(obj, dtgtk_gradient_slider_get_type())
#define DTGTK_IS_GRADIENT_SLIDER_CLASS(klass) G_TYPE_CHECK_CLASS_TYPE(obj, dtgtk_gradient_slider_get_type())
#define DTGTK_GRADIENT_SLIDER_MULTIVALUE(obj)                                                                \
  G_TYPE_CHECK_INSTANCE_CAST(obj, dtgtk_gradient_slider_multivalue_get_type(), GtkDarktableGradientSlider)
#define DTGTK_GRADIENT_SLIDER_MULTIVALUE_CLASS(klass)                                                        \
  G_TYPE_CHECK_CLASS_CAST(klass, dtgtk_gradient_slider_multivalue_get_type(), GtkDarktableGradientSliderClass)
#define DTGTK_IS_GRADIENT_SLIDER_MULTIVALUE(obj)                                                             \
  G_TYPE_CHECK_INSTANCE_TYPE(obj, dtgtk_gradient_slider_multivalue_get_type())
#define DTGTK_IS_GRADIENT_SLIDER_MULTIVALUE_CLASS(klass)                                                     \
  G_TYPE_CHECK_CLASS_TYPE(obj, dtgtk_gradient_slider_multivalue_get_type())

enum
{
  GRADIENT_SLIDER_VALUE_CHANGED,
  GRADIENT_SLIDER_LAST_SIGNAL
};

enum _gradient_slider_direction
{
  MOVE_LEFT = 0,
  MOVE_RIGHT = 1
};


/** bitfields for marker: bit-0 open/filled, bit-1 lower off/on, bit-2 upper off/on, bit-3 size small/big */
enum
{
  GRADIENT_SLIDER_MARKER_DOUBLE_OPEN = 0x06,
  GRADIENT_SLIDER_MARKER_DOUBLE_FILLED = 0x07,
  GRADIENT_SLIDER_MARKER_UPPER_OPEN = 0x04,
  GRADIENT_SLIDER_MARKER_UPPER_FILLED = 0x05,
  GRADIENT_SLIDER_MARKER_LOWER_OPEN = 0x02,
  GRADIENT_SLIDER_MARKER_LOWER_FILLED = 0x03,

  GRADIENT_SLIDER_MARKER_DOUBLE_OPEN_BIG = 0x0e,
  GRADIENT_SLIDER_MARKER_DOUBLE_FILLED_BIG = 0x0f,
  GRADIENT_SLIDER_MARKER_UPPER_OPEN_BIG = 0x0c,
  GRADIENT_SLIDER_MARKER_UPPER_FILLED_BIG = 0x0d,
  GRADIENT_SLIDER_MARKER_LOWER_OPEN_BIG = 0x0a,
  GRADIENT_SLIDER_MARKER_LOWER_FILLED_BIG = 0x0b
};

enum
{
  GRADIENT_SLIDER_MARGINS_DEFAULT = 6,
  GRADIENT_SLIDER_MARGINS_ZERO = 0,
  GRADIENT_SLIDER_MARGINS_SMALL = 2,
  GRADIENT_SLIDER_MARGINS_BIG = 6
};


typedef struct _GtkDarktableGradientSlider
{
  GtkWidget widget;
  GList *colors;
  gint selected;
  gdouble min, max;
  gint positions;
  gdouble position[GRADIENT_SLIDER_MAX_POSITIONS];
  gdouble resetvalue[GRADIENT_SLIDER_MAX_POSITIONS];
  gint marker[GRADIENT_SLIDER_MAX_POSITIONS];
  gdouble increment;
  gdouble picker[3];
  gint prev_x_root;
  gint margins;
  gboolean is_dragging;
  gboolean is_changed;
  gboolean is_resettable;
  gboolean do_reset;
  gboolean is_entered;
  guint timeout_handle;
} GtkDarktableGradientSlider;

typedef struct _GtkDarktableGradientSliderClass
{
  GtkWidgetClass parent_class;
} GtkDarktableGradientSliderClass;

typedef struct _gradient_slider_stop_t
{
  gdouble position;
  GdkRGBA color;
} _gradient_slider_stop_t;


GType dtgtk_gradient_slider_get_type(void);
GType dtgtk_gradient_slider_multivalue_get_type(void);

/** instantiate a new darktable gradient slider control */
GtkWidget *dtgtk_gradient_slider_new();
GtkWidget *dtgtk_gradient_slider_new_with_color(GdkRGBA start, GdkRGBA end);

/** Set a color at specified stop */
void dtgtk_gradient_slider_set_stop(GtkDarktableGradientSlider *gslider, gfloat position, GdkRGBA color);

/** Clear all stops */
void dtgtk_gradient_slider_multivalue_clear_stops(GtkDarktableGradientSlider *gslider);

/** Get the slider value 0 - 1.0*/
gdouble dtgtk_gradient_slider_get_value(GtkDarktableGradientSlider *gslider);
void dtgtk_gradient_slider_set_value(GtkDarktableGradientSlider *gslider, gdouble value);
gboolean dtgtk_gradient_slider_is_dragging(GtkDarktableGradientSlider *gslider);

/** Set the slider marker */
void dtgtk_gradient_slider_set_marker(GtkDarktableGradientSlider *gslider, gint mark);

/** Set the slider reset value */
void dtgtk_gradient_slider_set_resetvalue(GtkDarktableGradientSlider *gslider, gdouble value);

/** Set a picker */
void dtgtk_gradient_slider_set_picker(GtkDarktableGradientSlider *gslider, gdouble value);
void dtgtk_gradient_slider_set_picker_meanminmax(GtkDarktableGradientSlider *gslider, gdouble mean,
                                                 gdouble min, gdouble max);

/** Set left/right margins */
void dtgtk_gradient_slider_set_margins(GtkDarktableGradientSlider *gslider, gint value);

/** set increment for scroll action */
void dtgtk_gradient_slider_set_increment(GtkDarktableGradientSlider *gslider, gdouble value);


/** instantiate a new darktable gradient slider multivalue control */
GtkWidget *dtgtk_gradient_slider_multivalue_new(gint positions);
GtkWidget *dtgtk_gradient_slider_multivalue_new_with_color(GdkRGBA start, GdkRGBA end, gint positions);

/** Set a color at specified stop for multivalue control */
void dtgtk_gradient_slider_multivalue_set_stop(GtkDarktableGradientSlider *gslider, gfloat position,
                                               GdkRGBA color);

/** Get the slider value 0 - 1.0 for multivalue control */
gdouble dtgtk_gradient_slider_multivalue_get_value(GtkDarktableGradientSlider *gslider, gint position);
void dtgtk_gradient_slider_multivalue_get_values(GtkDarktableGradientSlider *gslider, gdouble *values);
void dtgtk_gradient_slider_multivalue_set_value(GtkDarktableGradientSlider *gslider, gdouble value,
                                                gint position);
void dtgtk_gradient_slider_multivalue_set_values(GtkDarktableGradientSlider *gslider, gdouble *values);
gboolean dtgtk_gradient_slider_multivalue_is_dragging(GtkDarktableGradientSlider *gslider);

/** Set the slider markers for multivalue control */
void dtgtk_gradient_slider_multivalue_set_marker(GtkDarktableGradientSlider *gslider, gint mark, gint pos);
void dtgtk_gradient_slider_multivalue_set_markers(GtkDarktableGradientSlider *gslider, gint *markers);

/** Set the slider reset values for multivalue control */
void dtgtk_gradient_slider_multivalue_set_resetvalue(GtkDarktableGradientSlider *gslider, gdouble value,
                                                     gint pos);
void dtgtk_gradient_slider_multivalue_set_resetvalues(GtkDarktableGradientSlider *gslider, gdouble *values);

/** Set a picker for multivalue control */
void dtgtk_gradient_slider_multivalue_set_picker(GtkDarktableGradientSlider *gslider, gdouble value);
void dtgtk_gradient_slider_multivalue_set_picker_meanminmax(GtkDarktableGradientSlider *gslider, gdouble mean,
                                                            gdouble min, gdouble max);

/** Set left/right margins */
void dtgtk_gradient_slider_multivalue_set_margins(GtkDarktableGradientSlider *gslider, gint value);

/** set increment for scroll action */
void dtgtk_gradient_slider_multivalue_set_increment(GtkDarktableGradientSlider *gslider, gdouble value);


G_END_DECLS

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
