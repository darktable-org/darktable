/*
    This file is part of darktable,
    copyright (c)2024 Aldric Renaudin.

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
#include "taglabel.h"
#include "gui/gtk.h"
#include <string.h>

G_DEFINE_TYPE(GtkDarktableTagLabel, dtgtk_tag_label, GTK_TYPE_LABEL);

static void dtgtk_tag_label_class_init(GtkDarktableTagLabelClass *klass)
{
  // GtkWidgetClass *widget_class = (GtkWidgetClass *)klass;
}

static void dtgtk_tag_label_init(GtkDarktableTagLabel *tag_label)
{
}

// Public functions
GtkWidget *dtgtk_tag_label_new(const gchar* text, const gint tagid)
{
  GtkDarktableTagLabel *tag_label;
  tag_label = g_object_new(dtgtk_tag_label_get_type(), NULL);

  tag_label->label = GTK_LABEL(gtk_label_new(text));
  tag_label->tagid = tagid;

  dt_gui_add_class(GTK_WIDGET(tag_label), "dt_tag_label");
  gtk_widget_set_name(GTK_WIDGET(tag_label), "tag_label");
  return (GtkWidget *)tag_label;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

