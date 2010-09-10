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

#include <control/control.h>
#include <common/darktable.h>

#include <stdlib.h>
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

static GtkEventBoxClass* _slider_parent_class;
void _slider_get_value_area(GtkWidget *widget,GdkRectangle *rect);
gdouble _slider_translate_value_to_pos(GtkAdjustment *adj,GdkRectangle *value_area,gdouble value);
gdouble _slider_translate_pos_to_value(GtkAdjustment *adj,GdkRectangle *value_area,gint x);

static void _slider_entry_abort(GtkDarktableSlider *slider);

static void _slider_class_init(GtkDarktableSliderClass *klass);
static void _slider_init(GtkDarktableSlider *scale);
static void _slider_size_request(GtkWidget *widget, GtkRequisition *requisition);
static void _slider_size_allocate(GtkWidget *widget, GtkAllocation *allocation);
static void _slider_realize(GtkWidget *widget);
static gboolean _slider_expose(GtkWidget *widget, GdkEventExpose *event);
//static void _slider_destroy(GtkObject *object);

// Slider Events
static gboolean _slider_button_press(GtkWidget *widget, GdkEventButton *event);
static gboolean _slider_button_release(GtkWidget *widget, GdkEventButton *event);
static gboolean _slider_scroll_event(GtkWidget *widget, GdkEventScroll *event);
static gboolean _slider_motion_notify(GtkWidget *widget, GdkEventMotion *event);
static gboolean _slider_enter_notify_event(GtkWidget *widget, GdkEventCrossing *event);

// Slider entry events
static gboolean _slider_entry_key_event(GtkWidget *widget,GdkEventKey *event, gpointer data);

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
  //GtkObjectClass *object_class=(GtkObjectClass *) klass;
  _slider_parent_class = gtk_type_class (gtk_event_box_get_type ());

  widget_class->realize = _slider_realize;
  widget_class->size_request = _slider_size_request;
  widget_class->size_allocate = _slider_size_allocate;
  widget_class->expose_event = _slider_expose;
  widget_class->button_press_event = _slider_button_press;
  widget_class->button_release_event = _slider_button_release;
  widget_class->scroll_event = _slider_scroll_event;
  widget_class->motion_notify_event = _slider_motion_notify;
  widget_class->enter_notify_event = _slider_enter_notify_event;
  widget_class->leave_notify_event = _slider_enter_notify_event;
  //object_class->destroy = _slider_destroy;
  _signals[VALUE_CHANGED] = g_signal_new(
    "value-changed",
     G_TYPE_OBJECT, G_SIGNAL_RUN_LAST,
    0,NULL,NULL,
     g_cclosure_marshal_VOID__VOID,
    GTK_TYPE_NONE,0);
}

static void _slider_init (GtkDarktableSlider *slider)
{
  slider->is_dragging=FALSE;
  slider->is_sensibility_key_pressed=FALSE;
  slider->entry=gtk_entry_new();
  
  gtk_widget_add_events (GTK_WIDGET (slider),
    GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | 
    GDK_ENTER_NOTIFY_MASK |  GDK_LEAVE_NOTIFY_MASK |
    GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK | 
    GDK_POINTER_MOTION_MASK);
  
  GtkWidget *hbox=gtk_hbox_new(TRUE,0);
  slider->hbox = GTK_HBOX(hbox);
  GtkWidget *left=gtk_label_new("");
  GtkWidget *right=gtk_label_new("");
  gtk_widget_set_size_request(left,20,0);
  gtk_widget_set_size_request(right,20,0);
  
  gtk_box_pack_start(GTK_BOX(hbox),left,FALSE,FALSE,0);
  gtk_box_pack_start(GTK_BOX(hbox),slider->entry,TRUE,TRUE,0);
  gtk_box_pack_start(GTK_BOX(hbox),right,FALSE,FALSE,0);
  
  gtk_container_add(GTK_CONTAINER(slider),hbox);
  //gtk_container_set_reallocate_redraws(GTK_CONTAINER(slider),TRUE);

  gtk_entry_set_has_frame (GTK_ENTRY(slider->entry), FALSE);
  
  g_signal_connect (G_OBJECT (slider->entry), "key-press-event", G_CALLBACK(_slider_entry_key_event), (gpointer)slider);
  
  dt_gui_key_accel_block_on_focus (slider->entry);
  
  //g_signal_connect (G_OBJECT (slider->entry), "size-allocate", G_CALLBACK(_slider_entry_size_allocate), (gpointer)slider);

}

static gboolean _slider_postponed_value_change(gpointer data) {
  gdk_threads_enter();
  if(DTGTK_SLIDER(data)->is_changed==TRUE)
  {
    g_signal_emit_by_name(G_OBJECT(data),"value-changed");
    if(DTGTK_SLIDER(data)->type==DARKTABLE_SLIDER_VALUE)
  DTGTK_SLIDER(data)->is_changed=FALSE;
  }
  gdk_threads_leave();
  return DTGTK_SLIDER(data)->is_dragging;	// This is called by the gtk mainloop and is threadsafe
}

static gboolean _slider_button_press(GtkWidget *widget, GdkEventButton *event)
{
  GtkDarktableSlider *slider=DTGTK_SLIDER(widget);
  if( event->button==3) 
  {
    char sv[32]={0};
    slider->is_entry_active=TRUE;
    gdouble value = gtk_adjustment_get_value(slider->adjustment);
    sprintf(sv,"%.*f",slider->digits,value);
    gtk_entry_set_text (GTK_ENTRY(slider->entry),sv);
    gtk_widget_show (GTK_WIDGET(slider->entry));
    gtk_widget_grab_focus (GTK_WIDGET(slider->entry));
    gtk_widget_queue_draw (widget);
  }
  else if( event->button==1 )
  {
    if( event->x > 0 && event->x < (DTGTK_SLIDER_ADJUST_BUTTON_WIDTH+(DTGTK_SLIDER_BORDER_WIDTH*2)) ) { 
      float value = gtk_adjustment_get_value(slider->adjustment)-(gtk_adjustment_get_step_increment(slider->adjustment));
      if(slider->snapsize) value = slider->snapsize * (((int)value)/slider->snapsize);
      gtk_adjustment_set_value(slider->adjustment, value);
      gtk_widget_draw(widget,NULL);
      g_signal_emit_by_name(G_OBJECT(widget),"value-changed");
    } 
    else if( event->x > (widget->allocation.width-(DTGTK_SLIDER_ADJUST_BUTTON_WIDTH+(DTGTK_SLIDER_BORDER_WIDTH*2))) && event->x < widget->allocation.width  ) 
    {
      float value = gtk_adjustment_get_value(slider->adjustment)+(gtk_adjustment_get_step_increment(slider->adjustment));
      if(slider->snapsize) value = slider->snapsize * (((int)value)/slider->snapsize);
      gtk_adjustment_set_value(slider->adjustment, value);
      gtk_widget_draw(widget,NULL);
      g_signal_emit_by_name(G_OBJECT(widget),"value-changed");
    }
    else
    {
      slider->is_dragging=TRUE;
      slider->prev_x_root=event->x_root;
      if( slider->type==DARKTABLE_SLIDER_BAR) slider->is_changed=TRUE;
      g_timeout_add(DTGTK_SLIDER_VALUE_CHANGED_DELAY, _slider_postponed_value_change,widget);
    }
  }
  return TRUE;
}

static gboolean _slider_button_release(GtkWidget *widget, GdkEventButton *event)
{
  GtkDarktableSlider *slider=DTGTK_SLIDER(widget);
  
  if( event->button==1 )
  {
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
        {
          float value = _slider_translate_pos_to_value(slider->adjustment, &vr, vmx);
          if(slider->snapsize) value = slider->snapsize * (((int)value)/slider->snapsize);
          gtk_adjustment_set_value(slider->adjustment, value);
        }
        gtk_widget_draw(widget,NULL);
        slider->prev_x_root = event->x_root;
      }
      slider->is_dragging=FALSE;
    }
  }  
  return TRUE;
}

static gboolean _slider_scroll_event(GtkWidget *widget, GdkEventScroll *event) 
{
  double inc=gtk_adjustment_get_step_increment(DTGTK_SLIDER(widget)->adjustment);
  DTGTK_SLIDER(widget)->is_sensibility_key_pressed = (event->state&GDK_CONTROL_MASK)?TRUE:FALSE;

  inc*= (DTGTK_SLIDER(widget)->is_sensibility_key_pressed==TRUE) ? 1.0 : DTGTK_VALUE_SENSITIVITY;
  float value = gtk_adjustment_get_value( DTGTK_SLIDER(widget)->adjustment ) + ((event->direction == GDK_SCROLL_UP || event->direction == GDK_SCROLL_RIGHT)?inc:-inc);
  if(DTGTK_SLIDER(widget)->snapsize) value = DTGTK_SLIDER(widget)->snapsize * (((int)value)/DTGTK_SLIDER(widget)->snapsize);
  gtk_adjustment_set_value(DTGTK_SLIDER(widget)->adjustment, value);
  gtk_widget_draw( widget, NULL);
  g_signal_emit_by_name(G_OBJECT(widget),"value-changed");
  return TRUE;
}

static void _slider_entry_commit(GtkDarktableSlider *slider) {
  gtk_widget_hide( slider->entry );
  gdouble value=atof(gtk_entry_get_text(GTK_ENTRY(slider->entry)));
  slider->is_entry_active=FALSE;
  dtgtk_slider_set_value(slider,value);
  gtk_widget_queue_draw(GTK_WIDGET(slider));
}

static void _slider_entry_abort(GtkDarktableSlider *slider) {
  gtk_widget_hide( slider->entry );
  slider->is_entry_active=FALSE;
  gtk_widget_queue_draw(GTK_WIDGET(slider));
}


static gboolean _slider_entry_key_event(GtkWidget* widget, GdkEventKey* event, gpointer data)
{
  if (event->keyval == GDK_Return || event->keyval == GDK_KP_Enter)
    _slider_entry_commit(DTGTK_SLIDER(data));
  if (event->keyval==GDK_Escape || event->keyval==GDK_Tab)
    _slider_entry_abort(DTGTK_SLIDER(data));
  else if( // Masking allowed keys...
              event->keyval == GDK_minus || event->keyval == GDK_KP_Subtract ||
              event->keyval == GDK_plus || event->keyval == GDK_KP_Add ||
              event->keyval == GDK_period ||
              event->keyval == GDK_Left  ||
              event->keyval == GDK_Right  ||
              event->keyval == GDK_Delete  ||
              event->keyval == GDK_BackSpace  ||
              event->keyval == GDK_0  || event->keyval == GDK_KP_0  ||
              event->keyval == GDK_1  || event->keyval == GDK_KP_1  ||
              event->keyval == GDK_2  || event->keyval == GDK_KP_2  ||
              event->keyval == GDK_3  || event->keyval == GDK_KP_3  ||
              event->keyval == GDK_4  || event->keyval == GDK_KP_4  ||
              event->keyval == GDK_5  || event->keyval == GDK_KP_5  ||
              event->keyval == GDK_6  || event->keyval == GDK_KP_6  ||
              event->keyval == GDK_7  || event->keyval == GDK_KP_7  ||
              event->keyval == GDK_8  || event->keyval == GDK_KP_8  ||
              event->keyval == GDK_9  || event->keyval == GDK_KP_9 
  ) 
  {
    return FALSE;
  }
  // Prevent all other keys withing entry
  return TRUE;
}

static gboolean _slider_motion_notify(GtkWidget *widget, GdkEventMotion *event)
{
  GtkDarktableSlider *slider=DTGTK_SLIDER(widget);
  slider->is_sensibility_key_pressed = (event->state&GDK_CONTROL_MASK)?TRUE:FALSE;
  if( slider->is_dragging==TRUE ) {
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
      
    if( slider->type==DARKTABLE_SLIDER_VALUE || ( slider->type==DARKTABLE_SLIDER_BAR && slider->is_sensibility_key_pressed==TRUE ) ) 
    {
      gdouble inc = gtk_adjustment_get_step_increment( slider->adjustment );
      if(DARKTABLE_SLIDER_VALUE &&  !slider->is_sensibility_key_pressed) inc*=DTGTK_VALUE_SENSITIVITY;
      gdouble value = gtk_adjustment_get_value(slider->adjustment) + ( ((slider->prev_x_root <= (gint)event->x_root) && slider->motion_direction==1 )?(inc):-(inc));
      if(slider->snapsize) value = slider->snapsize * (((int)value)/slider->snapsize);
      gtk_adjustment_set_value(slider->adjustment, value);
      slider->is_changed=TRUE;
    } 
    else if( slider->type==DARKTABLE_SLIDER_BAR )
    {
      if( vmx >= 0 && vmx <= vr.width)
      {
        float value = _slider_translate_pos_to_value(slider->adjustment, &vr, vmx);
        if(slider->snapsize) value = slider->snapsize * (((int)value)/slider->snapsize);
        gtk_adjustment_set_value(slider->adjustment, value);
      }
    }
      
      
    gtk_widget_draw(widget,NULL);
    slider->prev_x_root = event->x_root;
  }
  return FALSE;
}

static gboolean _slider_enter_notify_event(GtkWidget *widget, GdkEventCrossing *event) {
  gtk_widget_set_state(widget,(event->type == GDK_ENTER_NOTIFY)?GTK_STATE_PRELIGHT:GTK_STATE_NORMAL);
  gtk_widget_draw(widget,NULL);
  DTGTK_SLIDER(widget)->prev_x_root=event->x_root;
  return FALSE;
}

static void  _slider_size_request(GtkWidget *widget,GtkRequisition *requisition)
{
  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_SLIDER(widget));
  g_return_if_fail(requisition != NULL);
  
  GTK_WIDGET_CLASS(_slider_parent_class)->size_request(widget, requisition);

  requisition->width = 100;
  requisition->height = 20;
}

static void _slider_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_SLIDER(widget));
  g_return_if_fail(allocation != NULL);

  widget->allocation = *allocation;
  GTK_WIDGET_CLASS(_slider_parent_class)->size_allocate(widget, allocation);

  if (GTK_WIDGET_REALIZED(widget)) {
     gdk_window_move_resize(
         widget->window,
         allocation->x, allocation->y,
         allocation->width, allocation->height
     );
    
    if(DTGTK_SLIDER(widget)->is_entry_active == FALSE)
      gtk_widget_hide(DTGTK_SLIDER (widget)->entry);
   }
}

static void _slider_realize(GtkWidget *widget)
{
  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_SLIDER(widget));

  GdkWindowAttr attributes;
  guint attributes_mask;
  GtkWidgetClass* klass = GTK_WIDGET_CLASS (_slider_parent_class);
  
  if (klass->realize)
    klass->realize (widget);
  
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
  GtkStyle *style=gtk_rc_get_style_by_paths(gtk_settings_get_default(), NULL,"GtkButton", GTK_TYPE_BUTTON);
  GtkDarktableSlider *slider=DTGTK_SLIDER(widget);
  int state = gtk_widget_get_state(widget);
  int width = widget->allocation.width;
  int height = widget->allocation.height;

  if(width<=1) return FALSE;	// VERY STRANGE, expose seemed to be called before a widgetallocation has been made...
  
  if(slider->is_entry_active)
    state=GTK_STATE_PRELIGHT;

  // Widget bakground
  gtk_paint_box(style,widget->window, GTK_STATE_NORMAL,
   GTK_SHADOW_ETCHED_OUT,
   NULL,
   widget,
   "button",
   0,0,width,height); 
  
  // Value background
  GdkRectangle vr;
  _slider_get_value_area(widget,&vr);
  gtk_paint_box(style,widget->window, state,
   GTK_SHADOW_ETCHED_OUT,
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
    DTGTK_SLIDER_ADJUST_BUTTON_WIDTH-(DTGTK_SLIDER_BORDER_WIDTH), height-(DTGTK_SLIDER_BORDER_WIDTH*4),
    CPF_DIRECTION_LEFT);

  dtgtk_cairo_paint_arrow(cr,
    width-DTGTK_SLIDER_ADJUST_BUTTON_WIDTH-DTGTK_SLIDER_BORDER_WIDTH, DTGTK_SLIDER_BORDER_WIDTH*2,
    DTGTK_SLIDER_ADJUST_BUTTON_WIDTH-(DTGTK_SLIDER_BORDER_WIDTH), height-(DTGTK_SLIDER_BORDER_WIDTH*4),
    CPF_DIRECTION_RIGHT);

  if(slider->is_entry_active) {
    gtk_widget_draw(slider->entry, NULL);
    return TRUE;
  } else {
  
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
    switch(slider->fmt_type) 
    {
      case DARKTABLE_SLIDER_FORMAT_RATIO:
      {
        gdouble min=gtk_adjustment_get_lower(slider->adjustment);
        gdouble max=gtk_adjustment_get_upper(slider->adjustment);
        gdouble value=gtk_adjustment_get_value(slider->adjustment);
        double f= (value-min)/(max-min);
        sprintf(sv,"%.*f / %.*f",slider->digits,100.0*(1.0-f),slider->digits,100.0*f);
      }
      break;
      case DARKTABLE_SLIDER_FORMAT_PERCENT:
      {
   gdouble min=gtk_adjustment_get_lower(slider->adjustment);
         gdouble max=gtk_adjustment_get_upper(slider->adjustment);
         gdouble value=gtk_adjustment_get_value(slider->adjustment);
         double f= (value-min)/(max-min);   
         sprintf(sv,"%.*f %%",slider->digits,100.0*f);
      }
      break;
      case DARKTABLE_SLIDER_FORMAT_NONE:
      break;
      case DARKTABLE_SLIDER_FORMAT_FLOAT:
      default:
        sprintf(sv,"%.*f",slider->digits,value);
      break;
      
    }
    pango_layout_set_text(layout,sv,strlen(sv));
    GdkRectangle t={0,0,width,height};
    int pw,ph;
    pango_layout_get_pixel_size(layout,&pw,&ph);
    gtk_paint_layout(style,widget->window, state,TRUE,&t,widget,"label",(width/2.0) - (pw/2.0),(height/2.0)-(ph/2.0)+1,layout);
  }
  return FALSE;
}

/*
static void _slider_destroy(GtkObject *object)
{
  GtkDarktableSlider *slider;
  GtkDarktableSliderClass *klass;

  g_return_if_fail(object != NULL);
  g_return_if_fail(DTGTK_IS_SLIDER(object));

  slider = DTGTK_SLIDER(object);
  if( slider->key_snooper_id > 0 )
    gtk_key_snooper_remove (slider->key_snooper_id);
  
  if (GTK_IS_WIDGET (slider->entry)) gtk_widget_destroy (slider->entry);
  
  if (GTK_IS_WIDGET (slider->hbox)) gtk_widget_destroy (GTK_WIDGET(slider->hbox));

  
  klass = gtk_type_class(gtk_widget_get_type());

  if (GTK_OBJECT_CLASS(klass)->destroy) {
     (* GTK_OBJECT_CLASS(klass)->destroy) (object);
  }
}*/

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
  slider->is_entry_active=FALSE;
  slider->snapsize = 0;
  return GTK_WIDGET(slider);
}

void dtgtk_slider_set_digits(GtkDarktableSlider *slider, gint digits)
{
  slider->digits = digits;
}

void dtgtk_slider_set_snap(GtkDarktableSlider *slider, gint snapsize)
{
  slider->snapsize = snapsize;
}

void dtgtk_slider_set_format_type(GtkDarktableSlider *slider, darktable_slider_format_type_t type)
{
  slider->fmt_type=type;
}


gdouble dtgtk_slider_get_value(GtkDarktableSlider *slider) 
{
  return gtk_adjustment_get_value( slider->adjustment );
}

void dtgtk_slider_set_value(GtkDarktableSlider *slider, gdouble value)
{
  if(slider->snapsize) value = slider->snapsize * (((int)value)/slider->snapsize);
  gtk_adjustment_set_value( slider->adjustment, value );
  g_signal_emit_by_name(G_OBJECT(slider),"value-changed");
  gtk_widget_queue_draw(GTK_WIDGET(slider));
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
    dtgtk_slider_type = gtk_type_unique(GTK_TYPE_EVENT_BOX, &dtgtk_slider_info);
  }
  return dtgtk_slider_type;
}
