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
#include <math.h>
#include <cairo.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include "paint.h"
#include "slider.h"

#define DTGTK_SLIDER_ADJUST_BUTTON_WIDTH 12
#define DTGTK_SLIDER_BORDER_WIDTH 2
#define DTGTK_VALUE_SENSITIVITY 5.0
#define DTGTK_SLIDER_SENSIBILITY_KEY GDK_Control_L 
// Delay before emitting value change while draggin value.. (prevent hogging hostapp)
#define DTGTK_SLIDER_VALUE_CHANGED_DELAY 250

void _slider_get_value_area(GtkWidget *widget,GdkRectangle *rect);
gdouble _slider_translate_value_to_pos(GtkAdjustment *adj,GdkRectangle *value_area,gdouble value);
gdouble _slider_translate_pos_to_value(GtkAdjustment *adj,GdkRectangle *value_area,gint x);

static void _slider_class_init(GtkDarktableSliderClass *klass);
static void _slider_init(GtkDarktableSlider *scale);
static void _slider_size_request(GtkWidget *widget, GtkRequisition *requisition);
static void _slider_size_allocate(GtkWidget *widget, GtkAllocation *allocation);
static void _slider_realize(GtkWidget *widget);
static gboolean _slider_expose(GtkWidget *widget, GdkEventExpose *event);
static void _slider_destroy(GtkObject *object);

// Events
static gboolean _slider_button_press(GtkWidget *widget, GdkEventButton *event);
static gboolean _slider_button_release(GtkWidget *widget, GdkEventButton *event);
static gboolean _slider_motion_notify(GtkWidget *widget, GdkEventMotion *event);
static gboolean _slider_enter_notify_event(GtkWidget *widget, GdkEventCrossing *event);
static gboolean _slider_key_event(GtkWidget *widget, GdkEventKey *event);

static guint _signals[LAST_SIGNAL] = { 0 };

void _slider_get_value_area(GtkWidget *widget,GdkRectangle *rect) {
  rect->x = DTGTK_SLIDER_ADJUST_BUTTON_WIDTH+(DTGTK_SLIDER_BORDER_WIDTH*2);
  rect->y = DTGTK_SLIDER_BORDER_WIDTH;
  rect->width=widget->allocation.width-DTGTK_SLIDER_ADJUST_BUTTON_WIDTH-(DTGTK_SLIDER_BORDER_WIDTH*2)-rect->x;
  rect->height= widget->allocation.height-(DTGTK_SLIDER_BORDER_WIDTH*2);
}

gdouble _slider_translate_value_to_pos(GtkAdjustment *adj,GdkRectangle *value_area,gdouble value) 
{
  double lower=gtk_adjustment_get_lower(adj);
  double normrange =gtk_adjustment_get_upper(adj) - lower;
  gint barwidth=value_area->width;
  return barwidth*((value-lower)/normrange);
}

gdouble _slider_translate_pos_to_value(GtkAdjustment *adj,GdkRectangle *value_area,gint x) 
{	
  double value=0;
  double normrange = gtk_adjustment_get_upper(adj)-gtk_adjustment_get_lower(adj);
  gint barwidth=value_area->width;
  if( x > 0)
    value = (((double)x/(double)barwidth)*normrange);
  value+=gtk_adjustment_get_lower(adj);
  return value;
}

static void _slider_class_init (GtkDarktableSliderClass *klass)
{
  GtkWidgetClass *widget_class=(GtkWidgetClass *) klass;
  GtkObjectClass *object_class=(GtkObjectClass *) klass;
  widget_class->realize = _slider_realize;
  widget_class->size_request = _slider_size_request;
  widget_class->size_allocate = _slider_size_allocate;
  widget_class->expose_event = _slider_expose;
  widget_class->button_press_event = _slider_button_press;
  widget_class->button_release_event = _slider_button_release;
  widget_class->motion_notify_event = _slider_motion_notify;
  widget_class->enter_notify_event = _slider_enter_notify_event;
  widget_class->leave_notify_event = _slider_enter_notify_event;
  widget_class->key_press_event = _slider_key_event;
  widget_class->key_release_event = _slider_key_event;
  object_class->destroy = _slider_destroy;
  _signals[VALUE_CHANGED] = g_signal_new(
    "value-changed",
     G_TYPE_OBJECT, G_SIGNAL_RUN_LAST,
    0,NULL,NULL,
     g_cclosure_marshal_VOID__VOID,
    GTK_TYPE_NONE,0);
}

static gint _slider_key_snooper(GtkWidget *grab_widget,GdkEventKey *event,gpointer data)
{
  
  if( event->keyval == DTGTK_SLIDER_SENSIBILITY_KEY ) {
    if(DTGTK_IS_SLIDER(data)) 
    {
      GtkDarktableSlider *slider = DTGTK_SLIDER(data);
      slider->is_sensibility_key_pressed = (event->type==GDK_KEY_PRESS)?TRUE:FALSE;
    }
  }
  return FALSE;
}

static void _slider_init (GtkDarktableSlider *slider)
{
  slider->is_dragging=FALSE;
  slider->is_sensibility_key_pressed=FALSE;
  gtk_widget_add_events (GTK_WIDGET (slider),
    GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | 
    GDK_ENTER_NOTIFY_MASK |  GDK_LEAVE_NOTIFY_MASK |
    GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK | 
    GDK_POINTER_MOTION_MASK);
  
  gtk_key_snooper_install(_slider_key_snooper,slider);
  
}

static gboolean _slider_postponed_value_change(gpointer data) {
  g_signal_emit_by_name(G_OBJECT(data),"value-changed");
  return DTGTK_SLIDER(data)->is_dragging;	// This is called by the gtk mainloop and is threadsafe
}

static gdouble _slider_log=1.0;
static gboolean _slider_button_press(GtkWidget *widget, GdkEventButton *event)
{
  GtkDarktableSlider *slider=DTGTK_SLIDER(widget);

  if( event->x > 0 && event->x < (DTGTK_SLIDER_ADJUST_BUTTON_WIDTH+(DTGTK_SLIDER_BORDER_WIDTH*2))) {
    gtk_adjustment_set_value(slider->adjustment,gtk_adjustment_get_value(slider->adjustment)-(gtk_adjustment_get_step_increment(slider->adjustment)));
    gtk_widget_draw(widget,NULL);
    g_signal_emit_by_name(G_OBJECT(widget),"value-changed");
  } 
  else if( event->x > (widget->allocation.width-(DTGTK_SLIDER_ADJUST_BUTTON_WIDTH+(DTGTK_SLIDER_BORDER_WIDTH*2))) && event->x < widget->allocation.width ) 
  {
    gtk_adjustment_set_value(slider->adjustment,gtk_adjustment_get_value(slider->adjustment)+gtk_adjustment_get_step_increment(slider->adjustment));
    gtk_widget_draw(widget,NULL);
    g_signal_emit_by_name(G_OBJECT(widget),"value-changed");
  }
  else
  {
    slider->is_dragging=TRUE;
    slider->prev_x_root=event->x_root;
    _slider_log=1.0;
    g_timeout_add(DTGTK_SLIDER_VALUE_CHANGED_DELAY, _slider_postponed_value_change,widget);
  }
    return FALSE;
}

static gboolean _slider_button_release(GtkWidget *widget, GdkEventButton *event)
{
  GtkDarktableSlider *slider=DTGTK_SLIDER(widget);

  if( !( 
      event->x < (DTGTK_SLIDER_ADJUST_BUTTON_WIDTH+(DTGTK_SLIDER_BORDER_WIDTH*2)) && 
      event->x > widget->allocation.width-(DTGTK_SLIDER_ADJUST_BUTTON_WIDTH+(DTGTK_SLIDER_BORDER_WIDTH*2))
      )
  ) 
  {
    if( slider->type == DARKTABLE_SLIDER_BAR && !slider->is_sensibility_key_pressed ) 
    {
      // First get some dimention info
      GdkRectangle vr;
      _slider_get_value_area(widget,&vr);
      
      // Adjust rect to match dimensions for bar
      vr.x+=DTGTK_SLIDER_BORDER_WIDTH*2;
      vr.width-=(DTGTK_SLIDER_BORDER_WIDTH*4);
      gint vmx = event->x-vr.x;
      if( vmx >= 0 && vmx <= vr.width)
        gtk_adjustment_set_value(slider->adjustment, _slider_translate_pos_to_value(slider->adjustment, &vr, vmx));
      gtk_widget_draw(widget,NULL);
      slider->prev_x_root = event->x_root;
    }
    slider->is_dragging=FALSE;
    _slider_log=1.0;
  }
  return FALSE;
}

static gboolean _slider_key_event(GtkWidget *widget, GdkEventKey *event) 
{
  GtkDarktableSlider *slider=DTGTK_SLIDER(widget);
  if( event->type == GDK_KEY_PRESS && event->keyval==DTGTK_SLIDER_SENSIBILITY_KEY && !slider->is_ctrl_key_pressed ) 
    slider->is_ctrl_key_pressed = TRUE;
  else if( event->type == GDK_KEY_RELEASE && event->keyval==DTGTK_SLIDER_SENSIBILITY_KEY && slider->is_ctrl_key_pressed ) 
    slider->is_ctrl_key_pressed = FALSE;
  
  fprintf(stderr,"Ctrl key pressed state = %d\n",slider->is_ctrl_key_pressed );
  
  return TRUE;
}

static gboolean _slider_motion_notify(GtkWidget *widget, GdkEventMotion *event)
{
  GtkDarktableSlider *slider=DTGTK_SLIDER(widget);
  if( slider->is_dragging ) {
    // First get some dimention info
    GdkRectangle vr;
    _slider_get_value_area(widget,&vr);
    
    if( (slider->prev_x_root < (gint)event->x_root) )
      slider->motion_direction=1;
    else if( slider->prev_x_root > (gint)event->x_root )
      slider->motion_direction=-1;
    
    // Adjust rect to match dimensions for bar
    vr.x+=DTGTK_SLIDER_BORDER_WIDTH*2;
    vr.width-=(DTGTK_SLIDER_BORDER_WIDTH*4);
    gint vmx = event->x-vr.x;
      
    if( slider->type==DARKTABLE_SLIDER_VALUE || ( slider->type==DARKTABLE_SLIDER_BAR && slider->is_sensibility_key_pressed==TRUE ) ) {
      gdouble inc = gtk_adjustment_get_step_increment( slider->adjustment );
      if(DARKTABLE_SLIDER_VALUE &&  !slider->is_sensibility_key_pressed)
        inc*=DTGTK_VALUE_SENSITIVITY;
      
      gdouble value = gtk_adjustment_get_value(slider->adjustment);
      gtk_adjustment_set_value( slider->adjustment, value + ( ((slider->prev_x_root <= (gint)event->x_root) && slider->motion_direction==1 )?(inc):-(inc)) );
    } 
    else if( slider->type==DARKTABLE_SLIDER_BAR )
    {
      if( vmx >= 0 && vmx <= vr.width)
        gtk_adjustment_set_value(slider->adjustment, _slider_translate_pos_to_value(slider->adjustment, &vr, vmx));
    }
      
    gtk_widget_draw(widget,NULL);
    slider->prev_x_root = event->x_root;
  }
  return FALSE;
}

static gboolean _slider_enter_notify_event(GtkWidget *widget, GdkEventCrossing *event) {
  gtk_widget_set_state(widget,(event->type == GDK_ENTER_NOTIFY )?GTK_STATE_PRELIGHT:GTK_STATE_NORMAL);
  gtk_widget_draw(widget,NULL);
  DTGTK_SLIDER(widget)->prev_x_root=event->x_root;
  return FALSE;
}

static void  _slider_size_request(GtkWidget *widget,GtkRequisition *requisition)
{
  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_SLIDER(widget));
  g_return_if_fail(requisition != NULL);

  requisition->width = 100;
  requisition->height = 18;
}

static void _slider_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_SLIDER(widget));
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

static void _slider_realize(GtkWidget *widget)
{
  GdkWindowAttr attributes;
  guint attributes_mask;

  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_SLIDER(widget));

  GTK_WIDGET_SET_FLAGS(widget, GTK_REALIZED);

  attributes.window_type = GDK_WINDOW_CHILD;
  attributes.x = widget->allocation.x;
  attributes.y = widget->allocation.y;
  attributes.width = 100;
  attributes.height = 20;

  attributes.wclass = GDK_INPUT_OUTPUT;
  attributes.event_mask = gtk_widget_get_events(widget) | GDK_EXPOSURE_MASK;

  attributes_mask = GDK_WA_X | GDK_WA_Y;

  widget->window = gdk_window_new( gtk_widget_get_parent_window (widget->parent),& attributes, attributes_mask);
  gdk_window_set_user_data(widget->window, widget);
  widget->style = gtk_style_attach(widget->style, widget->window);
  gtk_style_set_background(widget->style, widget->window, GTK_STATE_NORMAL);
}

static gboolean _slider_expose(GtkWidget *widget, GdkEventExpose *event)
{
  g_return_val_if_fail(widget != NULL, FALSE);
  g_return_val_if_fail(DTGTK_IS_SLIDER(widget), FALSE);
  g_return_val_if_fail(event != NULL, FALSE);
  static GtkStyle *style=NULL;
  GtkDarktableSlider *slider=DTGTK_SLIDER(widget);
  int state = gtk_widget_get_state(widget);

  int width = widget->allocation.width;
  int height = widget->allocation.height;

  if (!style) {
  style=gtk_rc_get_style_by_paths(gtk_settings_get_default(), NULL,"GtkButton", GTK_TYPE_BUTTON);
  }
  
  // Widget bakground
  gtk_paint_box(style,widget->window, GTK_STATE_NORMAL,
   GTK_SHADOW_ETCHED_IN,
   NULL,
   widget,
   "button",
   0,0,width,height);
  
  // Value background
  GdkRectangle vr;
  _slider_get_value_area(widget,&vr);
  gtk_paint_box(style,widget->window, state,
   GTK_SHADOW_ETCHED_IN,
   NULL,
   widget,
   "button",
   vr.x,0,vr.width,height);
  
  // Begin cairo drawing 
  cairo_t *cr;
  cr = gdk_cairo_create(widget->window);

  // Draw arrows
  cairo_set_source_rgb(cr,
    style->fg[state].red/65535.0, 
    style->fg[state].green/65535.0, 
    style->fg[state].blue/65535.0
  );
  
  dtgtk_cairo_paint_arrow(cr,
    DTGTK_SLIDER_BORDER_WIDTH*2, DTGTK_SLIDER_BORDER_WIDTH*2,
    DTGTK_SLIDER_ADJUST_BUTTON_WIDTH-(DTGTK_SLIDER_BORDER_WIDTH), DTGTK_SLIDER_ADJUST_BUTTON_WIDTH-(DTGTK_SLIDER_BORDER_WIDTH),
    TRUE);

  dtgtk_cairo_paint_arrow(cr,
    width-DTGTK_SLIDER_ADJUST_BUTTON_WIDTH-DTGTK_SLIDER_BORDER_WIDTH, DTGTK_SLIDER_BORDER_WIDTH*2,
    DTGTK_SLIDER_ADJUST_BUTTON_WIDTH-(DTGTK_SLIDER_BORDER_WIDTH), DTGTK_SLIDER_ADJUST_BUTTON_WIDTH-(DTGTK_SLIDER_BORDER_WIDTH),
    FALSE);

  gfloat value = gtk_adjustment_get_value( slider->adjustment );

  // Draw valuebar 
  if( slider->type == DARKTABLE_SLIDER_BAR ) {
  // draw the bar fill
  vr.x+=DTGTK_SLIDER_BORDER_WIDTH*2;
  vr.width-=(DTGTK_SLIDER_BORDER_WIDTH*4);
  gint pos = _slider_translate_value_to_pos(slider->adjustment,&vr,value);
  cairo_set_source_rgb(cr,
    (style->bg[state].red/65535.0)*1.7, 
    (style->bg [state].green/65535.0)*1.7, 
    (style->bg[state].blue/65535.0)*1.7
  );
    
  cairo_rectangle(cr,vr.x,3, pos,height-6);
  cairo_fill(cr);
  }
  
  cairo_destroy(cr);
 
  // Formating the display of value and draw it...
  PangoLayout *layout;    
  layout = gtk_widget_create_pango_layout(widget,NULL);
  pango_layout_set_font_description(layout,style->font_desc);
  char sv[32]={0};
  sprintf(sv,"%.*f",slider->digits,value);
  pango_layout_set_text(layout,sv,strlen(sv));
  GdkRectangle t={0,0,width,height};
  int pw,ph;
  pango_layout_get_pixel_size(layout,&pw,&ph);
  gtk_paint_layout(style,widget->window, state,TRUE,&t,widget,"label",(width/2.0) - (pw/2.0),(height/2.0)-(ph/2.0)+1,layout);

  return FALSE;
}

static void _slider_destroy(GtkObject *object)
{
  GtkDarktableSlider *dtscale;
  GtkDarktableSliderClass *klass;

  g_return_if_fail(object != NULL);
  g_return_if_fail(DTGTK_IS_SLIDER(object));

  dtscale = DTGTK_SLIDER(object);

  klass = gtk_type_class(gtk_widget_get_type());

  if (GTK_OBJECT_CLASS(klass)->destroy) {
     (* GTK_OBJECT_CLASS(klass)->destroy) (object);
  }
}

// Public functions
GtkWidget *dtgtk_slider_new(GtkAdjustment *adjustment)
{
  GtkDarktableSlider *slider;	
  g_return_val_if_fail(adjustment == NULL || GTK_IS_ADJUSTMENT (adjustment), NULL);
  slider = gtk_type_new(dtgtk_slider_get_type());
  slider->adjustment = adjustment;
  return (GtkWidget *)slider;
}

GtkWidget *dtgtk_slider_new_with_range (darktable_slider_type_t type,gdouble min,gdouble max,gdouble step,gdouble value, gint digits)
{
  GtkAdjustment *adj = (GtkAdjustment *)gtk_adjustment_new (value, min, max, step, step, 0);
  GtkDarktableSlider *slider=DTGTK_SLIDER(dtgtk_slider_new(adj));
  slider->default_value=value;
  slider->type=type;
  slider->digits=digits;
  return GTK_WIDGET(slider);
}

void dtgtk_slider_set_digits(GtkDarktableSlider *slider, gint digits) {
  slider->digits = digits;
}


gdouble dtgtk_slider_get_value(GtkDarktableSlider *slider) 
{
  return gtk_adjustment_get_value( slider->adjustment );
}

void dtgtk_slider_set_value(GtkDarktableSlider *slider,gdouble value) {
  gtk_adjustment_set_value( slider->adjustment, value );
}

void dtgtk_slider_set_type(GtkDarktableSlider *slider,darktable_slider_type_t type) 
{
  slider->type = type;
  gtk_widget_draw( GTK_WIDGET(slider), NULL);
}

GtkType dtgtk_slider_get_type() 
{
  static GtkType dtgtk_slider_type = 0;
  if (!dtgtk_slider_type) {
    static const GtkTypeInfo dtgtk_slider_info = {
      "GtkDarktableSlider",
      sizeof(GtkDarktableSlider),
      sizeof(GtkDarktableSliderClass),
      (GtkClassInitFunc) _slider_class_init,
      (GtkObjectInitFunc) _slider_init,
      NULL,
      NULL,
      (GtkClassInitFunc) NULL
    };
    dtgtk_slider_type = gtk_type_unique(GTK_TYPE_WIDGET, &dtgtk_slider_info);
  }
  return dtgtk_slider_type;
}
