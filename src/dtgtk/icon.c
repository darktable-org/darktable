/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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
#include "icon.h"
#include "gui/gtk.h"
#include <string.h>

static void _icon_class_init(GtkDarktableIconClass *klass);
static void _icon_init(GtkDarktableIcon *icon);
static gboolean _icon_draw(GtkWidget *widget, cairo_t *cr);


static void _icon_class_init(GtkDarktableIconClass *klass)
{
  GtkWidgetClass *widget_class = (GtkWidgetClass *)klass;
  widget_class->draw = _icon_draw;
}

static void _icon_init(GtkDarktableIcon *icon)
{
}

static gboolean _icon_draw(GtkWidget *widget, cairo_t *cr)
{
  g_return_val_if_fail(widget != NULL, FALSE);
  g_return_val_if_fail(DTGTK_IS_ICON(widget), FALSE);

  /* begin cairo drawing */
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);

  GtkStateFlags state = gtk_widget_get_state_flags(widget);

  GdkRGBA fg_color;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gtk_style_context_get_color(context, state, &fg_color);

  gdk_cairo_set_source_rgba(cr, &fg_color);

  /* draw icon */
  if(DTGTK_ICON(widget)->icon)
    DTGTK_ICON(widget)->icon(cr, 0, 0, allocation.width, allocation.height, DTGTK_ICON(widget)->icon_flags,
                             DTGTK_ICON(widget)->icon_data);

  return FALSE;
}

// Public functions
GtkWidget *dtgtk_icon_new(DTGTKCairoPaintIconFunc paint, gint paintflags, void *paintdata)
{
  GtkDarktableIcon *icon;
  icon = g_object_new(dtgtk_icon_get_type(), NULL);
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(icon), FALSE);
  icon->icon = paint;
  icon->icon_flags = paintflags;
  icon->icon_data = paintdata;
  gtk_widget_set_name(GTK_WIDGET(icon), "dt-icon");
  return (GtkWidget *)icon;
}

GType dtgtk_icon_get_type()
{
  static GType dtgtk_icon_type = 0;
  if(!dtgtk_icon_type)
  {
    static const GTypeInfo dtgtk_icon_info = {
      sizeof(GtkDarktableIconClass), (GBaseInitFunc)NULL, (GBaseFinalizeFunc)NULL,
      (GClassInitFunc)_icon_class_init, NULL, /* class_finalize */
      NULL,                                   /* class_data */
      sizeof(GtkDarktableIcon), 0,            /* n_preallocs */
      (GInstanceInitFunc)_icon_init,
    };
    dtgtk_icon_type = g_type_register_static(GTK_TYPE_EVENT_BOX, "GtkDarktableIcon", &dtgtk_icon_info, 0);
  }
  return dtgtk_icon_type;
}

void dtgtk_icon_set_paint(GtkWidget *icon, DTGTKCairoPaintIconFunc paint, gint paintflags, void *paintdata)
{
  g_return_if_fail(icon != NULL);
  DTGTK_ICON(icon)->icon = paint;
  DTGTK_ICON(icon)->icon_flags = paintflags;
  DTGTK_ICON(icon)->icon_data = paintdata;
  gtk_widget_queue_draw(icon);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
