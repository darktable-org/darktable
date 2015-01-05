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
#define DTGTK_BUTTON(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, dtgtk_button_get_type(), GtkDarktableButton)
#define DTGTK_BUTTON_CLASS(klass)                                                                            \
  G_TYPE_CHECK_CLASS_CAST(klass, dtgtk_button_get_type(), GtkDarktableButtonClass)
#define DTGTK_IS_BUTTON(obj) G_TYPE_CHECK_INSTANCE_TYPE(obj, dtgtk_button_get_type())
#define DTGTK_IS_BUTTON_CLASS(klass) G_TYPE_CHECK_CLASS_TYPE(obj, dtgtk_button_get_type())

typedef enum _darktable_button_flags
{
  DARKTABLE_BUTTON_SHOW_LABEL = 1
} _darktable_button_flags_t;

typedef struct _GtkDarktableButton
{
  GtkButton widget;
  DTGTKCairoPaintIconFunc icon;
  gint icon_flags;
  GdkRGBA bg, fg;
} GtkDarktableButton;

typedef struct _GtkDarktableButtonClass
{
  GtkButtonClass parent_class;
} GtkDarktableButtonClass;

GType dtgtk_button_get_type(void);

/** Instansiate a new darktable button control passing paint function as content */
GtkWidget *dtgtk_button_new(DTGTKCairoPaintIconFunc paint, gint paintflags);
/** set the paint function for a button */
void dtgtk_button_set_paint(GtkDarktableButton *button, DTGTKCairoPaintIconFunc paint, gint paintflags);
/** overwrite the foreground color, or NULL to reset it */
void dtgtk_button_override_color(GtkDarktableButton *button, GdkRGBA *color);
/** overwrite the background color, or NULL to reset it */
void dtgtk_button_override_background_color(GtkDarktableButton *button, GdkRGBA *color);

G_END_DECLS
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
