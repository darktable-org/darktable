/*
    This file is part of darktable,
    copyright (c)2011 Henrik Andersson.

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
#include <string.h>
#include "icon.h"
#include "gui/gtk.h"

static void _icon_class_init(GtkDarktableIconClass *klass);
static void _icon_init(GtkDarktableIcon *icon);
static void _icon_size_request(GtkWidget *widget, GtkRequisition *requisition);
static gboolean _icon_expose(GtkWidget *widget, GdkEventExpose *event);


static void _icon_class_init(GtkDarktableIconClass *klass)
{
  GtkWidgetClass *widget_class = (GtkWidgetClass *)klass;
  widget_class->size_request = _icon_size_request;
  widget_class->expose_event = _icon_expose;
}

static void _icon_init(GtkDarktableIcon *icon)
{
}

static void _icon_size_request(GtkWidget *widget, GtkRequisition *requisition)
{
  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_ICON(widget));
  g_return_if_fail(requisition != NULL);
  requisition->width = DT_PIXEL_APPLY_DPI(17);
  requisition->height = DT_PIXEL_APPLY_DPI(17);
}

static gboolean _icon_expose(GtkWidget *widget, GdkEventExpose *event)
{
  g_return_val_if_fail(widget != NULL, FALSE);
  g_return_val_if_fail(DTGTK_IS_ICON(widget), FALSE);
  g_return_val_if_fail(event != NULL, FALSE);
  GtkStyle *style = gtk_widget_get_style(widget);
  int state = gtk_widget_get_state(widget);
  int border = 0;

  /* update paint flags depending of states */
  int flags = DTGTK_ICON(widget)->icon_flags;


  /* begin cairo drawing */
  cairo_t *cr;
  cr = gdk_cairo_create(gtk_widget_get_window(widget));

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int x = allocation.x;
  int y = allocation.y;
  int width = allocation.width;
  int height = allocation.height;

  /*
      cairo_rectangle (cr,x,y,width,height);
      cairo_set_source_rgba (cr,
                             style->bg[state].red/65535.0,
                             style->bg[state].green/65535.0,
                             style->bg[state].blue/65535.0,
                             0.5);
      cairo_fill (cr);
  */

  cairo_set_source_rgb(cr, style->fg[state].red / 65535.0, style->fg[state].green / 65535.0,
                       style->fg[state].blue / 65535.0);

  /* draw icon */
  if(DTGTK_ICON(widget)->icon)
    DTGTK_ICON(widget)->icon(cr, x + border, y + border, width - (border * 2), height - (border * 2), flags);

  cairo_destroy(cr);

  return FALSE;
}

// Public functions
GtkWidget *dtgtk_icon_new(DTGTKCairoPaintIconFunc paint, gint paintflags)
{
  GtkDarktableIcon *icon;
  icon = g_object_new(dtgtk_icon_get_type(), NULL);
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(icon), FALSE);
  icon->icon = paint;
  icon->icon_flags = paintflags;
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

void dtgtk_icon_set_paint(GtkWidget *icon, DTGTKCairoPaintIconFunc paint, gint paintflags)
{
  DTGTK_ICON(icon)->icon = paint;
  DTGTK_ICON(icon)->icon_flags = paintflags;
  gtk_widget_queue_draw(icon);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
