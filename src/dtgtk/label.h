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
#ifndef DTGTK_LABEL_H
#define DTGTK_LABEL_H

#include <gtk/gtk.h>

G_BEGIN_DECLS
#define DTGTK_LABEL(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, dtgtk_label_get_type(), GtkDarktableLabel)
#define DTGTK_LABEL_CLASS(klass) GTK_CHECK_CLASS_CAST(klass, dtgtk_label_get_type(), GtkDarktableLabelClass)
#define DTGTK_IS_LABEL(obj) G_TYPE_CHECK_INSTANCE_TYPE(obj, dtgtk_label_get_type())
#define DTGTK_IS_LABEL_CLASS(klass) GTK_CHECK_CLASS_TYPE(obj, dtgtk_label_get_type())

typedef enum _darktable_label_flags
{
  DARKTABLE_LABEL_UNDERLINED = 1,
  DARKTABLE_LABEL_BACKFILLED = 2,
  DARKTABLE_LABEL_TAB = 4,
  DARKTABLE_LABEL_ALIGN_LEFT = 16,
  DARKTABLE_LABEL_ALIGN_RIGHT = 32,
  DARKTABLE_LABEL_ALIGN_CENTER = 64

} _darktable_label_flags_t;

typedef struct _GtkDarktableLabel
{
  GtkLabel widget;
  gint flags;
} GtkDarktableLabel;

typedef struct _GtkDarktableLabelClass
{
  GtkButtonClass parent_class;
} GtkDarktableLabelClass;

GType dtgtk_label_get_type(void);

/** Instansiate a new darktable label control passing paint function as content */
GtkWidget *dtgtk_label_new(const gchar *label, _darktable_label_flags_t flags);
/** set the text of the label */
void dtgtk_label_set_text(GtkDarktableLabel *label, const gchar *text, _darktable_label_flags_t flags);
G_END_DECLS
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
