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
#ifndef DTGTK_BUTTON_H
#define DTGTK_BUTTON_H

#include <gtk/gtk.h>
#include "paint.h"
G_BEGIN_DECLS
#define DTGTK_BUTTON(obj) GTK_CHECK_CAST(obj, dtgtk_button_get_type (), GtkDarktableButton)
#define DTGTK_BUTTON_CLASS(klass) GTK_CHECK_CLASS_CAST(klass, dtgtk_button_get_type(), GtkDarktableButtonClass)
#define DTGTK_IS_BUTTON(obj) GTK_CHECK_TYPE(obj, dtgtk_button_get_type())
#define DTGTK_IS_BUTTON_CLASS(klass) GTK_CHECK_CLASS_TYPE(obj, dtgtk_button_get_type())

typedef enum _darktable_button_flags
{
  DARKTABLE_BUTTON_SHOW_LABEL				= 1
}
_darktable_button_flags_t;

typedef struct _GtkDarktableButton
{
  GtkButton widget;
  DTGTKCairoPaintIconFunc icon;
  gint icon_flags;
} GtkDarktableButton;

typedef struct _GtkDarktableButtonClass
{
  GtkButtonClass parent_class;
} GtkDarktableButtonClass;

GType dtgtk_button_get_type (void);

/** Instansiate a new darktable button control passing paint function as content */
GtkWidget* dtgtk_button_new(DTGTKCairoPaintIconFunc paint, gint paintflags);
GtkWidget* dtgtk_button_new_with_label(const gchar *label, DTGTKCairoPaintIconFunc paint, gint paintflags);
/** add unmapped accelerators to increase and decrease slider */
void gtk_button_set_accel(GtkButton *button, GtkAccelGroup *accel_group, const gchar *accel_path);
void dtgtk_button_set_accel(GtkDarktableButton *button, GtkAccelGroup *accel_group, const gchar *accel_path);
/** register the slider shortcuts, can be called before the slider is created */
void gtk_button_init_accel(GtkAccelGroup *accel_group, const gchar *accel_path);
void dtgtk_button_init_accel(GtkAccelGroup *accel_group, const gchar *accel_path);
/** set the paint function for a button */
void dtgtk_button_set_paint(GtkDarktableButton *button,
                            DTGTKCairoPaintIconFunc paint,
                            gint paintflags);

G_END_DECLS
#endif
