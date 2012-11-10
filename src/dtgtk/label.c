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
#include "label.h"

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
  requisition->height = 17;
}

/*static void _label_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_LABEL(widget));
  g_return_if_fail(allocation != NULL);

  widget->allocation = *allocation;

  if (GTK_WIDGET_REALIZED(widget)) {
     gdk_window_move_resize(
         widget->window,
         allocation->x, allocation->y,
         allocation->width, allocation->height
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
  GtkStyle *style=gtk_rc_get_style_by_paths(gtk_settings_get_default(), NULL,"GtkButton", GTK_TYPE_BUTTON);
  if(!style) style = gtk_rc_get_style(widget);
  // uninitialized?
  if(style->depth == -1) return FALSE;
  int state = gtk_widget_get_state(widget);

  int x = widget->allocation.x;
  int y = widget->allocation.y;
  int width = widget->allocation.width;
  int height = widget->allocation.height;

  // Formating the display of text and draw it...
  PangoLayout *layout;
  layout = gtk_widget_create_pango_layout(widget,NULL);
  pango_layout_set_font_description(layout,style->font_desc);
  const gchar *text=gtk_label_get_text(GTK_LABEL(widget));
  pango_layout_set_text(layout,text,-1);
  GdkRectangle t= {x,y,x+width,y+height};
  int pw,ph;
  pango_layout_get_pixel_size(layout,&pw,&ph);


  // Begin cairo drawing

  cairo_t *cr;
  cr = gdk_cairo_create(widget->window);

  cairo_set_source_rgba(cr,
                        /* style->fg[state].red/65535.0,
                         style->fg[state].green/65535.0,
                         style->fg[state].blue/65535.0,*/
                        1,1,1,
                        0.10
                       );

  cairo_set_antialias(cr,CAIRO_ANTIALIAS_NONE);

  cairo_set_line_width(cr,1.0);
  cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);
  if( DTGTK_LABEL(widget)->flags&DARKTABLE_LABEL_UNDERLINED )
  {
    cairo_move_to(cr,x,y+height-2);
    cairo_line_to(cr,x+width,y+height-2);
    cairo_stroke(cr);
  }
  else if( DTGTK_LABEL(widget)->flags&DARKTABLE_LABEL_BACKFILLED )
  {
    cairo_rectangle(cr,x,y,width,height);
    cairo_fill(cr);
  }
  else if( DTGTK_LABEL(widget)->flags&DARKTABLE_LABEL_TAB )
  {
    int rx=x,rw=pw+2;
    if( DTGTK_LABEL(widget)->flags&DARKTABLE_LABEL_ALIGN_RIGHT ) rx=x+width-pw-8;
    cairo_rectangle(cr,rx,y,rw+4,height-1);
    cairo_fill(cr);

    if( DTGTK_LABEL(widget)->flags&DARKTABLE_LABEL_ALIGN_RIGHT )
    {
      // /|
      cairo_move_to(cr,x+width-rw-6,y);
      cairo_line_to(cr,x+width-rw-6-15,y+height-2);
      cairo_line_to(cr,x+width-rw-6,y+height-2);
      cairo_fill(cr);

      // hline
      cairo_move_to(cr,x,y+height-1);
      cairo_line_to(cr,x+width-rw-6,y+height-1);
      cairo_stroke(cr);
    }
    else
    {
      // |
      cairo_move_to(cr,x+rw+4,y);
      cairo_line_to(cr,x+rw+4+15,y+height-2);
      cairo_line_to(cr,x+rw+4,y+height-2);
      cairo_fill(cr);

      // hline
      cairo_move_to(cr,x+rw+4,y+height-1);
      cairo_line_to(cr,x+width,y+height-1);
      cairo_stroke(cr);
    }
  }
  cairo_set_antialias(cr,CAIRO_ANTIALIAS_DEFAULT);
  cairo_destroy(cr);

  // draw text
  int lx=x+4, ly=y+((height/2.0)-(ph/2.0));
  if( DTGTK_LABEL(widget)->flags&DARKTABLE_LABEL_ALIGN_RIGHT ) lx=x+width-pw-6;
  else if( DTGTK_LABEL(widget)->flags&DARKTABLE_LABEL_ALIGN_CENTER ) lx=(width/2.0)-(pw/2.0);
  gtk_paint_layout(style,widget->window, state,TRUE,&t,widget,"label",lx,ly,layout);

  return FALSE;
}

// Public functions
GtkWidget* dtgtk_label_new(const gchar *text, _darktable_label_flags_t flags)
{
  GtkDarktableLabel *label;
  label = gtk_type_new(dtgtk_label_get_type());
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

GtkType dtgtk_label_get_type()
{
  static GtkType dtgtk_label_type = 0;
  if (!dtgtk_label_type)
  {
    static const GtkTypeInfo dtgtk_label_info =
    {
      "GtkDarktableLabel",
      sizeof(GtkDarktableLabel),
      sizeof(GtkDarktableLabelClass),
      (GtkClassInitFunc) _label_class_init,
      (GtkObjectInitFunc) _label_init,
      NULL,
      NULL,
      (GtkClassInitFunc) NULL
    };
    dtgtk_label_type = gtk_type_unique(GTK_TYPE_LABEL, &dtgtk_label_info);
  }
  return dtgtk_label_type;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
