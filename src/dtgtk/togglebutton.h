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
#ifndef DTGTK_TOGGLEBUTTON_H
#define DTGTK_TOGGLEBUTTON_H

#include <gtk/gtk.h>
#include "paint.h"
G_BEGIN_DECLS
#define DTGTK_TOGGLEBUTTON(obj) GTK_CHECK_CAST(obj, dtgtk_togglebutton_get_type (), GtkDarktableToggleButton)
#define DTGTK_TOGGLEBUTTON_CLASS(klass) GTK_CHECK_CLASS_CAST(klass, dtgtk_togglebutton_get_type(), GtkDarktableToggleButtonClass)
#define DTGTK_IS_TOGGLEBUTTON(obj) GTK_CHECK_TYPE(obj, dtgtk_togglebutton_get_type())
#define DTGTK_IS_TOGGLEBUTTON_CLASS(klass) GTK_CHECK_CLASS_TYPE(obj, dtgtk_togglebutton_get_type())

typedef struct _GtkDarktableToggleButton
{
  GtkToggleButton widget;
  DTGTKCairoPaintIconFunc icon;
  gint icon_flags;
} GtkDarktableToggleButton;

typedef struct _GtkDarktableToggleButtonClass
{
  GtkToggleButtonClass parent_class;
} GtkDarktableToggleButtonClass;

GType dtgtk_togglebutton_get_type (void);

/** Instansiate a new darktable toggle button */
GtkWidget* dtgtk_togglebutton_new (DTGTKCairoPaintIconFunc paint, gint paintflag);
GtkWidget* dtgtk_togglebutton_new_with_label (const gchar *label,DTGTKCairoPaintIconFunc paint, gint paintflag);

/** Set the paint function and paint flags */
void dtgtk_togglebutton_set_paint(GtkDarktableToggleButton *button,
                            DTGTKCairoPaintIconFunc paint,
                            gint paintflags);
G_END_DECLS
#endif
