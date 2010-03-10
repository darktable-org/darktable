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

#include "togglebutton.h"

static void _togglebutton_class_init(GtkDarktableToggleButtonClass *klass);
static void _togglebutton_init(GtkDarktableToggleButton *slider);
static void _togglebutton_size_request(GtkWidget *widget, GtkRequisition *requisition);
static void _togglebutton_size_allocate(GtkWidget *widget, GtkAllocation *allocation);
//static void _togglebutton_realize(GtkWidget *widget);
static gboolean _togglebutton_expose(GtkWidget *widget, GdkEventExpose *event);
static void _togglebutton_destroy(GtkObject *object);

void temp() {
  _togglebutton_size_allocate(NULL,NULL);
  _togglebutton_size_request(NULL,NULL);
  _togglebutton_expose(NULL,NULL);
  _togglebutton_destroy(NULL);
  
}

static void _togglebutton_class_init (GtkDarktableToggleButtonClass *klass)
{
  GtkWidgetClass *widget_class=(GtkWidgetClass *) klass;
  //GtkObjectClass *object_class=(GtkObjectClass *) klass;
  //widget_class->realize = _togglebutton_realize;
  widget_class->size_request = _togglebutton_size_request;
  //widget_class->size_allocate = _togglebutton_size_allocate;
  widget_class->expose_event = _togglebutton_expose;
  //object_class->destroy = _togglebutton_destroy;
}

static void _togglebutton_init(GtkDarktableToggleButton *slider)
{
}

static void  _togglebutton_size_request(GtkWidget *widget,GtkRequisition *requisition)
{
  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_TOGGLEBUTTON(widget));
  g_return_if_fail(requisition != NULL);
  requisition->width = 18;
  requisition->height = 18;
}

static void _togglebutton_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_TOGGLEBUTTON(widget));
  g_return_if_fail(allocation != NULL);

  widget->allocation = *allocation;

  if (GTK_WIDGET_REALIZED(widget)) {
     gdk_window_move_resize(
         widget->window,
         allocation->x, allocation->y,
         allocation->width, allocation->height
     );
   }
}

/*
static void _togglebutton_realize(GtkWidget *widget)
{
  GdkWindowAttr attributes;
  guint attributes_mask;

  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_TOGGLEBUTTON(widget));

  GTK_WIDGET_SET_FLAGS(widget, GTK_REALIZED);

  attributes.window_type = GDK_WINDOW_CHILD;
  attributes.x = widget->allocation.x;
  attributes.y = widget->allocation.y;
  attributes.width = 18;
  attributes.height = 18;

  attributes.wclass = GDK_INPUT_OUTPUT;
  attributes.event_mask = gtk_widget_get_events(widget) | GDK_EXPOSURE_MASK;

  attributes_mask = GDK_WA_X | GDK_WA_Y;

  widget->window = gdk_window_new( gtk_widget_get_parent_window (widget->parent),& attributes, attributes_mask);
  gdk_window_set_user_data(widget->window, widget);
  widget->style = gtk_style_attach(widget->style, widget->window);
  gtk_style_set_background(widget->style, widget->window, GTK_STATE_NORMAL);
}*/

static void _togglebutton_destroy(GtkObject *object)
{
  GtkDarktableToggleButtonClass *klass;
  g_return_if_fail(object != NULL);
  g_return_if_fail(DTGTK_IS_TOGGLEBUTTON(object));  
  klass = gtk_type_class(gtk_widget_get_type());
  if (GTK_OBJECT_CLASS(klass)->destroy) {
     (* GTK_OBJECT_CLASS(klass)->destroy) (object);
  }
}

static gboolean _togglebutton_expose(GtkWidget *widget, GdkEventExpose *event)
{
  g_return_val_if_fail(widget != NULL, FALSE);
  g_return_val_if_fail(DTGTK_IS_TOGGLEBUTTON(widget), FALSE);
  g_return_val_if_fail(event != NULL, FALSE);
  static GtkStyle *style=NULL;
  int state = gtk_widget_get_state(widget);

  int x = widget->allocation.x;
  int y = widget->allocation.y;
  int width = widget->allocation.width;
  int height = widget->allocation.height;

  if (!style) {
    style=gtk_rc_get_style_by_paths(gtk_settings_get_default(), NULL,"GtkToggleButton", GTK_TYPE_BUTTON);
  }
 
  // Widget bakground
  gtk_paint_box(style,widget->window, state,
   GTK_SHADOW_ETCHED_IN,
   NULL,
   widget,
   "button",
   x,y,width,height);

  // Begin cairo drawing 
  cairo_t *cr;
  cr = gdk_cairo_create(widget->window);
   cairo_set_source_rgb(cr,
    style->fg[state].red/65535.0, 
    style->fg[state].green/65535.0, 
    style->fg[state].blue/65535.0
  );
 
  DTGTK_TOGGLEBUTTON(widget)->icon(cr,x+4,y+4,width-8,height-8,DTGTK_TOGGLEBUTTON(widget)->icon_flag);

  cairo_destroy(cr);
  return FALSE;
}

// Public functions
GtkWidget* dtgtk_togglebutton_new(DTGTKCairoPaintIconFunc paint, gboolean paintflag)
{
  GtkDarktableToggleButton *button;	
  button = gtk_type_new(dtgtk_togglebutton_get_type());
  button->icon=paint;
  button->icon_flag=paintflag;
  return (GtkWidget *)button;
}

GtkType dtgtk_togglebutton_get_type() 
{
  static GtkType dtgtk_togglebutton_type = 0;
  if (!dtgtk_togglebutton_type) {
    static const GtkTypeInfo dtgtk_togglebutton_info = {
      "GtkDarktableToggleButton",
      sizeof(GtkDarktableToggleButton),
      sizeof(GtkDarktableToggleButtonClass),
      (GtkClassInitFunc) _togglebutton_class_init,
      (GtkObjectInitFunc) _togglebutton_init,
      NULL,
      NULL,
      (GtkClassInitFunc) NULL
    };
    dtgtk_togglebutton_type = gtk_type_unique(GTK_TYPE_TOGGLE_BUTTON, &dtgtk_togglebutton_info);
  }
  return dtgtk_togglebutton_type;
}
