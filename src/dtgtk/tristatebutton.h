/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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
#ifndef DTGTK_TRISTATEBUTTON_H
#define DTGTK_TRISTATEBUTTON_H

#include <gtk/gtk.h>
#include "paint.h"
G_BEGIN_DECLS
#define DTGTK_TRISTATEBUTTON(obj)                                                                            \
  G_TYPE_CHECK_INSTANCE_CAST(obj, dtgtk_tristatebutton_get_type(), GtkDarktableTriStateButton)
#define DTGTK_TRISTATEBUTTON_CLASS(klass)                                                                    \
  GTK_CHECK_CLASS_CAST(klass, dtgtk_tristatebutton_get_type(), GtkDarktableTriStateButtonClass)
#define DTGTK_IS_TRISTATEBUTTON(obj) G_TYPE_CHECK_INSTANCE_TYPE(obj, dtgtk_tristatebutton_get_type())
#define DTGTK_IS_TRISTATEBUTTON_CLASS(klass) GTK_CHECK_CLASS_TYPE(obj, dtgtk_tristatebutton_get_type())

enum
{
  STATE_CHANGED,
  TRISTATEBUTTON_LAST_SIGNAL
};


typedef struct _GtkDarktableTriStateButton
{
  GtkButton widget;
  DTGTKCairoPaintIconFunc icon;
  gint icon_flags;
  gint state;
} GtkDarktableTriStateButton;

typedef struct _GtkDarktableTriStateButtonClass
{
  GtkButtonClass parent_class;
  void (*state_changed)(GtkDarktableTriStateButton *ts, int state);
} GtkDarktableTriStateButtonClass;

GType dtgtk_tristatebutton_get_type(void);

/** Instansiate a new darktable slider control passing adjustment as range */
GtkWidget *dtgtk_tristatebutton_new(DTGTKCairoPaintIconFunc paint, gint paintflag);
GtkWidget *dtgtk_tristatebutton_new_with_label(const gchar *label, DTGTKCairoPaintIconFunc paint,
                                               gint paintflag);
/** get the current state of the tristate button */
gint dtgtk_tristatebutton_get_state(const GtkDarktableTriStateButton *);
/** set the current state of the tristate button */
void dtgtk_tristatebutton_set_state(GtkDarktableTriStateButton *, gint state);

G_END_DECLS
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
