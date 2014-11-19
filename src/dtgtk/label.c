/*
    This file is part of darktable,
    copyright (c)2010 Henrik Andersson.

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
#include "common/darktable.h"
#include "gui/gtk.h"
#include "label.h"
#include "bauhaus/bauhaus.h"

static void _label_class_init(GtkDarktableLabelClass *klass);
static void _label_init(GtkDarktableLabel *slider);
static void _label_size_request(GtkWidget *widget, GtkRequisition *requisition);
//static void _label_size_allocate(GtkWidget *widget, GtkAllocation *allocation);
//static void _label_realize(GtkWidget *widget);
static gboolean _label_expose(GtkWidget *widget, GdkEventExpose *event);
//static void _label_destroy(GtkObject *object);


static void _label_class_init (GtkDarktableLabelClass *klass)
{
  GtkWidgetClass *widget_class=(GtkWidgetClass *) klass;
  //GtkObjectClass *object_class=(GtkObjectClass *) klass;
  //widget_class->realize = _label_realize;
  widget_class->size_request = _label_size_request;
  //widget_class->size_allocate = _label_size_allocate;
  widget_class->expose_event = _label_expose;
  //object_class->destroy = _label_destroy;
}

static void _label_init(GtkDarktableLabel *label)
{
}

static void  _label_size_request(GtkWidget *widget,GtkRequisition *requisition)
{
  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_LABEL(widget));
  g_return_if_fail(requisition != NULL);
  requisition->width = -1;
  requisition->height = DT_PIXEL_APPLY_DPI(17);
}

/*static void _label_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_LABEL(widget));
  g_return_if_fail(allocation != NULL);

  widget->allocation = *allocation;

  if (gtk_widget_get_realized(widget)) {
     gdk_window_move_resize(
         gtk_widget_get_window(widget),
         allocation.x, allocation.y,
         allocation.width, allocation.height
     );
   }
}*/

/*
static void _label_destroy(GtkObject *object)
{
  GtkDarktableLabelClass *klass;
  g_return_if_fail(object != NULL);
  g_return_if_fail(DTGTK_IS_LABEL(object));
  klass = gtk_type_class(gtk_widget_get_type());
  if (GTK_OBJECT_CLASS(klass)->destroy) {
     (* GTK_OBJECT_CLASS(klass)->destroy) (object);
  }
}*/

static gboolean _label_expose(GtkWidget *widget, GdkEventExpose *event)
{
  g_return_val_if_fail(widget != NULL, FALSE);
  g_return_val_if_fail(DTGTK_IS_LABEL(widget), FALSE);
  g_return_val_if_fail(event != NULL, FALSE);

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int x = allocation.x;
  int y = allocation.y;
  int width = allocation.width;
  int height = allocation.height;

  // Formatting the display of text and draw it...
  PangoLayout *layout;
  layout = gtk_widget_create_pango_layout(widget,NULL);
  pango_layout_set_font_description(layout, darktable.bauhaus->pango_font_desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
  const gchar *text=gtk_label_get_text(GTK_LABEL(widget));
  pango_layout_set_text(layout,text,-1);

  int pw,ph;
  pango_layout_get_pixel_size(layout,&pw,&ph);


  // Begin cairo drawing

  cairo_t *cr;
  cr = gdk_cairo_create(gtk_widget_get_window(widget));

  cairo_set_source_rgba(cr, 1,1,1, 0.10);

  cairo_set_antialias(cr,CAIRO_ANTIALIAS_NONE);

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
//   cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);
  if( DTGTK_LABEL(widget)->flags&DARKTABLE_LABEL_UNDERLINED )
  {
    // TODO: make DPI aware
    cairo_move_to(cr,x,y+height-2);
    cairo_line_to(cr,x+width,y+height-2);
    cairo_stroke(cr);
  }
  else if( DTGTK_LABEL(widget)->flags&DARKTABLE_LABEL_BACKFILLED )
  {
    // TODO: make DPI aware
    cairo_rectangle(cr,x,y,width,height);
    cairo_fill(cr);
  }
  else if( DTGTK_LABEL(widget)->flags&DARKTABLE_LABEL_TAB )
  {
    float rx = x,
          rw = pw + DT_PIXEL_APPLY_DPI(2);

    // the blocks
    if( DTGTK_LABEL(widget)->flags&DARKTABLE_LABEL_ALIGN_RIGHT )
    {
      rx = x + width - pw - DT_PIXEL_APPLY_DPI(8.0);

      cairo_move_to(cr, rx + rw + DT_PIXEL_APPLY_DPI(4.0), y + height - DT_PIXEL_APPLY_DPI(1.0));
      cairo_line_to(cr, rx + rw + DT_PIXEL_APPLY_DPI(4.0), y);
      cairo_line_to(cr, rx, y);
      cairo_line_to(cr, rx - DT_PIXEL_APPLY_DPI(15.0), y + height - DT_PIXEL_APPLY_DPI(1.0));
      cairo_close_path(cr);
      cairo_fill(cr);
    }
    else
    {
      cairo_move_to(cr, rx, y);
      cairo_line_to(cr, rx + rw + DT_PIXEL_APPLY_DPI(4.0), y);
      cairo_line_to(cr, rx + rw + DT_PIXEL_APPLY_DPI(4.0 + 15.0), y + height - DT_PIXEL_APPLY_DPI(1.0));
      cairo_line_to(cr, rx, y + height - DT_PIXEL_APPLY_DPI(1.0));
      cairo_close_path(cr);
      cairo_fill(cr);

    }

    // hline
    cairo_move_to(cr, x, y + height - DT_PIXEL_APPLY_DPI(0.5));
    cairo_line_to(cr, x + width - DT_PIXEL_APPLY_DPI(2.0), y + height - DT_PIXEL_APPLY_DPI(0.5));
    cairo_stroke(cr);
  }

  // draw text
  int lx=x+DT_PIXEL_APPLY_DPI(4), ly=y+((height/2.0)-(ph/2.0));
  if( DTGTK_LABEL(widget)->flags&DARKTABLE_LABEL_ALIGN_RIGHT ) lx=x+width-pw-DT_PIXEL_APPLY_DPI(6);
  else if( DTGTK_LABEL(widget)->flags&DARKTABLE_LABEL_ALIGN_CENTER ) lx=(width/2.0)-(pw/2.0);
  cairo_move_to(cr, lx, ly);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
  pango_cairo_show_layout(cr, layout);
  g_object_unref(layout);

  cairo_set_antialias(cr,CAIRO_ANTIALIAS_DEFAULT);
  cairo_destroy(cr);

  return FALSE;
}

// Public functions
GtkWidget* dtgtk_label_new(const gchar *text, _darktable_label_flags_t flags)
{
  GtkDarktableLabel *label;
  label = g_object_new(dtgtk_label_get_type(), NULL);
  gtk_label_set_text(GTK_LABEL(label),text);
  label->flags=flags;
  return (GtkWidget *)label;
}

void dtgtk_label_set_text(GtkDarktableLabel *label,
                          const gchar *text,
                          _darktable_label_flags_t flags)
{
  gtk_label_set_text(GTK_LABEL(label),text);
  label->flags=flags;
  gtk_widget_queue_draw(GTK_WIDGET(label));
}

GType dtgtk_label_get_type()
{
  static GType dtgtk_label_type = 0;
  if (!dtgtk_label_type)
  {
    static const GTypeInfo dtgtk_label_info =
    {
      sizeof(GtkDarktableLabelClass),
      (GBaseInitFunc) NULL,
      (GBaseFinalizeFunc) NULL,
      (GClassInitFunc) _label_class_init,
      NULL,           /* class_finalize */
      NULL,           /* class_data */
      sizeof(GtkDarktableLabel),
      0,              /* n_preallocs */
      (GInstanceInitFunc) _label_init,
    };
    dtgtk_label_type = g_type_register_static(GTK_TYPE_LABEL, "GtkDarktableLabel", &dtgtk_label_info, 0);
  }
  return dtgtk_label_type;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
