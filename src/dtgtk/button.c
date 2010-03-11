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

#include "button.h"

static void _button_class_init(GtkDarktableButtonClass *klass);
static void _button_init(GtkDarktableButton *slider);
static void _button_size_request(GtkWidget *widget, GtkRequisition *requisition);
//static void _button_size_allocate(GtkWidget *widget, GtkAllocation *allocation);
//static void _button_realize(GtkWidget *widget);
static gboolean _button_expose(GtkWidget *widget, GdkEventExpose *event);
//static void _button_destroy(GtkObject *object);


static void _button_class_init (GtkDarktableButtonClass *klass)
{
  GtkWidgetClass *widget_class=(GtkWidgetClass *) klass;
  //GtkObjectClass *object_class=(GtkObjectClass *) klass;
  //widget_class->realize = _button_realize;
  widget_class->size_request = _button_size_request;
  //widget_class->size_allocate = _button_size_allocate;
  widget_class->expose_event = _button_expose;
  //object_class->destroy = _button_destroy;
}

static void _button_init(GtkDarktableButton *slider)
{
}

static void  _button_size_request(GtkWidget *widget,GtkRequisition *requisition)
{
  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_BUTTON(widget));
  g_return_if_fail(requisition != NULL);
  requisition->width = 17;
  requisition->height = 17;
}

/*static void _button_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_BUTTON(widget));
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
static void _button_destroy(GtkObject *object)
{
  GtkDarktableButtonClass *klass;
  g_return_if_fail(object != NULL);
  g_return_if_fail(DTGTK_IS_BUTTON(object));  
  klass = gtk_type_class(gtk_widget_get_type());
  if (GTK_OBJECT_CLASS(klass)->destroy) {
     (* GTK_OBJECT_CLASS(klass)->destroy) (object);
  }
}*/

static gboolean _button_expose(GtkWidget *widget, GdkEventExpose *event)
{
  g_return_val_if_fail(widget != NULL, FALSE);
  g_return_val_if_fail(DTGTK_IS_BUTTON(widget), FALSE);
  g_return_val_if_fail(event != NULL, FALSE);
  static GtkStyle *style=NULL;
  int state = gtk_widget_get_state(widget);

  int x = widget->allocation.x;
  int y = widget->allocation.y;
  int width = widget->allocation.width;
  int height = widget->allocation.height;

  if (!style) {
    style=gtk_rc_get_style_by_paths(gtk_settings_get_default(), NULL,"GtkButton", GTK_TYPE_BUTTON);
  }
 
  // Begin cairo drawing 
  cairo_t *cr;
  cr = gdk_cairo_create(widget->window);
  
  // draw background dependent on state
  if( state != GTK_STATE_NORMAL )
  {
    cairo_rectangle(cr,x,y,width,height);
    cairo_set_source_rgba(cr,
      style->bg[state].red/65535.0, 
      style->bg[state].green/65535.0, 
      style->bg[state].blue/65535.0,
      0.5
    );
    cairo_fill(cr);
  }

  // draw icon
  cr = gdk_cairo_create(widget->window);
   cairo_set_source_rgb(cr,
    style->fg[state].red/65535.0, 
    style->fg[state].green/65535.0, 
    style->fg[state].blue/65535.0
  );
  
  DTGTK_BUTTON(widget)->icon(cr,x+2,y+2,width-4,height-4,DTGTK_BUTTON(widget)->icon_flag);

  cairo_destroy(cr);
  return FALSE;
}

// Public functions
GtkWidget* dtgtk_button_new(DTGTKCairoPaintIconFunc paint, gboolean paintflag)
{
  GtkDarktableButton *button;	
  button = gtk_type_new(dtgtk_button_get_type());
  button->icon=paint;
  button->icon_flag=paintflag;
  return (GtkWidget *)button;
}

GtkType dtgtk_button_get_type() 
{
  static GtkType dtgtk_button_type = 0;
  if (!dtgtk_button_type) {
    static const GtkTypeInfo dtgtk_button_info = {
      "GtkDarktableButton",
      sizeof(GtkDarktableButton),
      sizeof(GtkDarktableButtonClass),
      (GtkClassInitFunc) _button_class_init,
      (GtkObjectInitFunc) _button_init,
      NULL,
      NULL,
      (GtkClassInitFunc) NULL
    };
    dtgtk_button_type = gtk_type_unique(GTK_TYPE_BUTTON, &dtgtk_button_info);
  }
  return dtgtk_button_type;
}
