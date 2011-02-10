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
#ifndef DTGTK_GRADIENT_SLIDER_H
#define DTGTK_GRADIENT_SLIDER_H

#include <gtk/gtk.h>
#include "paint.h"
G_BEGIN_DECLS
#define DTGTK_GRADIENT_SLIDER(obj) GTK_CHECK_CAST(obj, dtgtk_gradient_slider_get_type (), GtkDarktableGradientSlider)
#define DTGTK_GRADIENT_SLIDER_CLASS(klass) GTK_CHECK_CLASS_CAST(klass, dtgtk_gradient_slider_get_type(), GtkDarktableGradientSliderClass)
#define DTGTK_IS_GRADIENT_SLIDER(obj) GTK_CHECK_TYPE(obj, dtgtk_gradient_slider_get_type())
#define DTGTK_IS_GRADIENT_SLIDER_CLASS(klass) GTK_CHECK_CLASS_TYPE(obj, dtgtk_gradient_slider_get_type())

enum
{
  GRADIENT_SLIDER_VALUE_CHANGED,
  GRADIENT_SLIDER_LAST_SIGNAL
};

typedef struct _GtkDarktableGradientSlider
{
  GtkWidget widget;
  GList *colors;
  gdouble position;
  gint prev_x_root;
  gboolean is_dragging;
  gboolean is_changed;
} GtkDarktableGradientSlider;

typedef struct _GtkDarktableGradientSliderClass
{
  GtkWidgetClass parent_class;
} GtkDarktableGradientSliderClass;

GType dtgtk_gradient_slider_get_type (void);

/** Instansiate a new darktable gradient slider control */
GtkWidget* dtgtk_gradient_slider_new();
GtkWidget* dtgtk_gradient_slider_new_with_color(GdkColor start,GdkColor end);

/** Set a color at specified stop */
void dtgtk_gradient_slider_set_stop(GtkDarktableGradientSlider *gslider,gfloat position,GdkColor color);

/** Get the slider value 0 - 1.0*/
gdouble dtgtk_gradient_slider_get_value(GtkDarktableGradientSlider *gslider);
void dtgtk_gradient_slider_set_value(GtkDarktableGradientSlider *gslider,gdouble value);
gboolean dtgtk_gradient_slider_is_dragging(GtkDarktableGradientSlider *gslider);
G_END_DECLS
#endif
