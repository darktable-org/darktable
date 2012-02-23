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
#include <assert.h>

#include "gradientslider.h"

//#define DTGTK_GRADIENT_SLIDER_VALUE_CHANGED_DELAY 250

typedef struct _gradient_slider_stop_t
{
  gdouble position;
  GdkColor color;
}
_gradient_slider_stop_t;

static void _gradient_slider_class_init(GtkDarktableGradientSliderClass *klass);
static void _gradient_slider_init(GtkDarktableGradientSlider *slider);
static void _gradient_slider_size_request(GtkWidget *widget, GtkRequisition *requisition);
//static void _gradient_slider_size_allocate(GtkWidget *widget, GtkAllocation *allocation);
static void _gradient_slider_realize(GtkWidget *widget);
static gboolean _gradient_slider_expose(GtkWidget *widget, GdkEventExpose *event);
static void _gradient_slider_destroy(GtkObject *object);

// Events
static gboolean _gradient_slider_enter_notify_event(GtkWidget *widget, GdkEventCrossing *event);
static gboolean _gradient_slider_button_press(GtkWidget *widget, GdkEventButton *event);
static gboolean _gradient_slider_button_release(GtkWidget *widget, GdkEventButton *event);
static gboolean _gradient_slider_motion_notify(GtkWidget *widget, GdkEventMotion *event);

enum {
  VALUE_CHANGED,
  LAST_SIGNAL
};

static guint _signals[LAST_SIGNAL] = { 0 };

/*static gboolean _gradient_slider_postponed_value_change(gpointer data) {
  gdk_threads_enter();
  if(DTGTK_GRADIENT_SLIDER(data)->is_changed==TRUE)
  {
    g_signal_emit_by_name(G_OBJECT(data),"value-changed");
    DTGTK_GRADIENT_SLIDER(data)->is_changed=FALSE;
  }
  gdk_threads_leave();
  return DTGTK_GRADIENT_SLIDER(data)->is_dragging;	// This is called by the gtk mainloop and is threadsafe
}*/

static gboolean _gradient_slider_enter_notify_event(GtkWidget *widget, GdkEventCrossing *event)
{
  gtk_widget_set_state(widget,(event->type == GDK_ENTER_NOTIFY)?GTK_STATE_PRELIGHT:GTK_STATE_NORMAL);
  gtk_widget_draw(widget,NULL);
  DTGTK_GRADIENT_SLIDER(widget)->prev_x_root=event->x_root;
  return FALSE;
}

static gboolean _gradient_slider_button_press(GtkWidget *widget, GdkEventButton *event)
{
  GtkDarktableGradientSlider *gslider=DTGTK_GRADIENT_SLIDER(widget);

  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS && gslider->is_resettable)
  {
    gslider->is_dragging=FALSE;
    gslider->do_reset=TRUE;
    for(int k=0; k<gslider->positions; k++) gslider->position[k] = gslider->resetvalue[k];
    gtk_widget_draw(widget,NULL);
    g_signal_emit_by_name(G_OBJECT(widget),"value-changed");
  }
  else if( event->button==1)
  {
    gslider->is_dragging=TRUE;
    gslider->do_reset=FALSE;
    gslider->prev_x_root=event->x_root;
    gdouble newposition = event->x / widget->allocation.width;
    gslider->selected = -1;

    assert(gslider->positions > 0);

    if(gslider->positions == 1)
    {
      gslider->selected = 0;
      gslider->min = 0.0;
      gslider->max = 1.0;
    }
    else if (newposition <= gslider->position[0])
    {
      gslider->selected = 0;
      gslider->min = 0.0;
      gslider->max = gslider->position[1];
    }
    else if (newposition >= gslider->position[gslider->positions-1])
    {
      gslider->selected = gslider->positions-1;
      gslider->min = gslider->position[gslider->positions-2];
      gslider->max = 1.0f;
    }
    else for(int k=0; k<=gslider->positions-2; k++)
    {
      if(newposition >= gslider->position[k] && newposition <= gslider->position[k+1])
      {
        gslider->selected = newposition - gslider->position[k] < gslider->position[k+1] - newposition ? k : k+1;
        gslider->min = gslider->selected == 0 ? 0.0f : gslider->position[gslider->selected-1];
        gslider->max = gslider->selected == gslider->positions-1 ? 1.0f : gslider->position[gslider->selected+1];
        break;
      }
    }

    assert(gslider->selected >= 0);
    assert(gslider->selected <= gslider->positions-1);

    newposition = newposition > 1.0 ? 1.0 : newposition < 0.0 ? 0.0 : newposition;
    gslider->position[gslider->selected] = newposition > gslider->max ? gslider->max : (newposition < gslider->min ? gslider->min : newposition);
    g_signal_emit_by_name(G_OBJECT(widget),"value-changed");

    // Emitt value change signal
    //gslider->is_changed=TRUE;
    //g_timeout_add(DTGTK_GRADIENT_SLIDER_VALUE_CHANGED_DELAY, _gradient_slider_postponed_value_change,widget);
  }
  return TRUE;
}

static gboolean _gradient_slider_motion_notify(GtkWidget *widget, GdkEventMotion *event)
{
  GtkDarktableGradientSlider *gslider=DTGTK_GRADIENT_SLIDER(widget);
  if( gslider->is_dragging==TRUE && gslider->do_reset==FALSE )
  {
    gdouble newposition = event->x / widget->allocation.width;
    gslider->position[gslider->selected] = newposition > gslider->max ? gslider->max : (newposition < gslider->min ? gslider->min : newposition);
    //gslider->is_changed=TRUE;
    g_signal_emit_by_name(G_OBJECT(widget),"value-changed");

    gtk_widget_draw(widget,NULL);
  }
  return TRUE;
}

static gboolean _gradient_slider_button_release(GtkWidget *widget, GdkEventButton *event)
{
  GtkDarktableGradientSlider *gslider=DTGTK_GRADIENT_SLIDER(widget);
  if( event->button==1  && gslider->do_reset==FALSE )
  {
    // First get some dimention info
    gslider->is_changed=TRUE;
    gdouble newposition = event->x / widget->allocation.width;
    gslider->position[gslider->selected] = newposition > gslider->max ? gslider->max : (newposition < gslider->min ? gslider->min : newposition);
    gtk_widget_draw(widget,NULL);
    gslider->prev_x_root = event->x_root;
    gslider->is_dragging=FALSE;
    g_signal_emit_by_name(G_OBJECT(widget),"value-changed");

  }
  return TRUE;
}

static void _gradient_slider_class_init (GtkDarktableGradientSliderClass *klass)
{
  GtkWidgetClass *widget_class=(GtkWidgetClass *) klass;
  GtkObjectClass *object_class=(GtkObjectClass *) klass;
  widget_class->realize = _gradient_slider_realize;
  widget_class->size_request = _gradient_slider_size_request;
  //widget_class->size_allocate = _gradient_slider_size_allocate;
  widget_class->expose_event = _gradient_slider_expose;
  object_class->destroy = _gradient_slider_destroy;

  widget_class->enter_notify_event = _gradient_slider_enter_notify_event;
  widget_class->leave_notify_event = _gradient_slider_enter_notify_event;
  widget_class->button_press_event = _gradient_slider_button_press;
  widget_class->button_release_event = _gradient_slider_button_release;
  widget_class->motion_notify_event = _gradient_slider_motion_notify;

  _signals[VALUE_CHANGED] = g_signal_new(
   "value-changed",
   G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
   0,NULL,NULL,
    g_cclosure_marshal_VOID__VOID,
   GTK_TYPE_NONE,0);
}

static void _gradient_slider_init(GtkDarktableGradientSlider *slider)
{
  slider->prev_x_root=slider->is_dragging=slider->is_changed=slider->do_reset=0;
}

static void  _gradient_slider_size_request(GtkWidget *widget,GtkRequisition *requisition)
{
  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_GRADIENT_SLIDER(widget));
  g_return_if_fail(requisition != NULL);
  requisition->width = 100;
  requisition->height = 17;
}



/*static void _gradient_slider_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
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

static void _gradient_slider_realize(GtkWidget *widget)
{
  GdkWindowAttr attributes;
  guint attributes_mask;

  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_GRADIENT_SLIDER(widget));

  GTK_WIDGET_SET_FLAGS(widget, GTK_REALIZED);

  attributes.window_type = GDK_WINDOW_CHILD;
  attributes.x = widget->allocation.x;
  attributes.y = widget->allocation.y;
  attributes.width = 100;
  attributes.height = 17;

  attributes.wclass = GDK_INPUT_OUTPUT;
  attributes.event_mask = gtk_widget_get_events(widget) | GDK_EXPOSURE_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_ENTER_NOTIFY_MASK |  GDK_LEAVE_NOTIFY_MASK | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK | GDK_POINTER_MOTION_MASK;
  attributes_mask = GDK_WA_X | GDK_WA_Y;

  widget->window = gdk_window_new(
                     gtk_widget_get_parent_window (widget),
                     & attributes, attributes_mask
                   );

  gdk_window_set_user_data(widget->window, widget);

  widget->style = gtk_style_attach(widget->style, widget->window);
  gtk_style_set_background(widget->style, widget->window, GTK_STATE_NORMAL);
}


static void _gradient_slider_destroy(GtkObject *object)
{
  GtkDarktableGradientSliderClass *klass;
  g_return_if_fail(object != NULL);
  g_return_if_fail(DTGTK_IS_GRADIENT_SLIDER(object));
  // TODO: Free colorentries
  klass = gtk_type_class(gtk_widget_get_type());
  if (GTK_OBJECT_CLASS(klass)->destroy)
  {
    (* GTK_OBJECT_CLASS(klass)->destroy) (object);
  }
}

static gboolean _gradient_slider_expose(GtkWidget *widget, GdkEventExpose *event)
{
  assert(DTGTK_GRADIENT_SLIDER(widget)->position > 0);

  g_return_val_if_fail(widget != NULL, FALSE);
  g_return_val_if_fail(DTGTK_IS_GRADIENT_SLIDER(widget), FALSE);
  g_return_val_if_fail(event != NULL, FALSE);
  GtkStyle *style=gtk_rc_get_style_by_paths(gtk_settings_get_default(), NULL,"GtkButton", GTK_TYPE_BUTTON);
  int state = gtk_widget_get_state(widget);

  /*int x = widget->allocation.x;
  int y = widget->allocation.y;*/
  int width = widget->allocation.width;
  int height = widget->allocation.height;

  // Begin cairo drawing
  cairo_t *cr;
  cr = gdk_cairo_create(widget->window);

  // First build the cairo gradient and then fill the gradient
  float gheight=height/2.0;
  float gwidth=width-1;
  GList *current=NULL;
  cairo_pattern_t *gradient=NULL;
  if((current=g_list_first(DTGTK_GRADIENT_SLIDER(widget)->colors)) != NULL)
  {
    gradient=cairo_pattern_create_linear(0,0,gwidth,gheight);
    do
    {
      _gradient_slider_stop_t *stop=(_gradient_slider_stop_t *)current->data;
      cairo_pattern_add_color_stop_rgb(gradient,stop->position,stop->color.red/65535.0,stop->color.green/65535.0,stop->color.blue/65535.0);
    }
    while((current=g_list_next(current))!=NULL);
  }

  if(gradient!=NULL) // Do we got a gradient, lets draw it
  {
    cairo_set_line_width(cr,0.1);
    cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);
    cairo_set_source(cr,gradient);
    cairo_rectangle(cr,0,gheight-(gheight/2.0),gwidth,gheight);
    cairo_fill(cr);
    cairo_stroke(cr);
  }

  // Lets draw position arrows

  cairo_set_source_rgba(cr,
                        style->fg[state].red/65535.0,
                        style->fg[state].green/65535.0,
                        style->fg[state].blue/65535.0,
                        1.0
                       );

  for(int k=0; k<DTGTK_GRADIENT_SLIDER(widget)->positions; k++)
  {
    int vx=gwidth*DTGTK_GRADIENT_SLIDER(widget)->position[k];
    int mk=DTGTK_GRADIENT_SLIDER(widget)->marker[k];
    int sz=(mk & (1<<3)) ? 15 : 5;  // big or small marker?

    if(sz < 10)
    {
      // supporting line for small markers
      cairo_move_to(cr,vx,2);
      cairo_line_to(cr,vx,height-2);
      cairo_set_antialias(cr,CAIRO_ANTIALIAS_NONE);
      cairo_set_line_width(cr,1.0);
      cairo_stroke(cr);
    }

    cairo_set_antialias(cr,CAIRO_ANTIALIAS_DEFAULT);

    if(mk & 0x04) /* upper arrow */
    {
      if (mk & 0x01) /* filled */
        dtgtk_cairo_paint_solid_triangle(cr, vx-sz/2, sz < 10 ? 1 : -2,sz,sz,CPF_DIRECTION_DOWN);
      else
        dtgtk_cairo_paint_triangle(cr, vx-sz/2, sz < 10 ? 1 : -2,sz,sz,CPF_DIRECTION_DOWN);
    }

    if(mk & 0x02) /* lower arrow */
    {
      if (mk & 0x01) /* filled */
        dtgtk_cairo_paint_solid_triangle(cr, vx-sz/2,sz < 10 ? height-6 : height-13,sz,sz,CPF_DIRECTION_UP);
      else
        dtgtk_cairo_paint_triangle(cr, vx-sz/2,sz < 10 ? height-6 : height-13,sz,sz,CPF_DIRECTION_UP);
    }
  }

  cairo_destroy(cr);
  return FALSE;
}

// Public functions for multivalue type
GtkWidget* dtgtk_gradient_slider_multivalue_new(gint positions)
{
  assert(positions <= GRADIENT_SLIDER_MAX_POSITIONS);

  GtkDarktableGradientSlider *gslider;
  gslider = gtk_type_new(dtgtk_gradient_slider_get_type());
  gslider->positions=positions;
  gslider->is_resettable = FALSE;
  for(int k=0; k<positions; k++) gslider->position[k] = 0.0;
  for(int k=0; k<positions; k++) gslider->resetvalue[k] = 0.0;
  for(int k=0; k<positions; k++) gslider->marker[k] = GRADIENT_SLIDER_MARKER_DOUBLE_OPEN;
  return (GtkWidget *)gslider;
}


GtkWidget* dtgtk_gradient_slider_multivalue_new_with_color(GdkColor start,GdkColor end, gint positions)
{
  assert(positions <= GRADIENT_SLIDER_MAX_POSITIONS);

  GtkDarktableGradientSlider *gslider;
  gslider = gtk_type_new(dtgtk_gradient_slider_get_type());
  gslider->positions=positions;
  gslider->is_resettable = FALSE;
  for(int k=0; k<positions; k++) gslider->position[k] = 0.0;
  for(int k=0; k<positions; k++) gslider->resetvalue[k] = 0.0;
  for(int k=0; k<positions; k++) gslider->marker[k] = GRADIENT_SLIDER_MARKER_DOUBLE_OPEN;

  // Construct gradient start color
  _gradient_slider_stop_t *gc=(_gradient_slider_stop_t *)g_malloc(sizeof(_gradient_slider_stop_t));
  gc->position=0.0;
  memcpy(&gc->color,&start,sizeof(GdkColor));
  gslider->colors = g_list_append(gslider->colors , gc);

  // Construct gradient stop color
  gc=(_gradient_slider_stop_t *)g_malloc(sizeof(_gradient_slider_stop_t));
  gc->position=1.0;
  memcpy(&gc->color,&end,sizeof(GdkColor));
  gslider->colors = g_list_append(gslider->colors , gc);


  return (GtkWidget *)gslider;
}


gint _list_find_by_position(gconstpointer a, gconstpointer b)
{
  _gradient_slider_stop_t *stop=(_gradient_slider_stop_t *)a;
  gfloat position=*((gfloat *)b);
  return (gint)((stop->position*100.0) - (position*100.0));
}

void dtgtk_gradient_slider_multivalue_set_stop(GtkDarktableGradientSlider *gslider,gfloat position,GdkColor color)
{
  // First find color att position, if exists update color, otherwise create a new stop at position.
  GList *current=g_list_find_custom(gslider->colors,(gpointer)&position,_list_find_by_position);
  if( current != NULL )
  {
    memcpy(&((_gradient_slider_stop_t*)current->data)->color,&color,sizeof(GdkColor));
  }
  else
  {
    // stop didn't exist lets add it
    _gradient_slider_stop_t *gc=(_gradient_slider_stop_t *)g_malloc(sizeof(_gradient_slider_stop_t));
    gc->position=position;
    memcpy(&gc->color,&color,sizeof(GdkColor));
    gslider->colors = g_list_append(gslider->colors , gc);
  }
}

GtkType dtgtk_gradient_slider_multivalue_get_type()
{
  static GtkType dtgtk_gradient_slider_type = 0;
  if (!dtgtk_gradient_slider_type)
  {
    static const GtkTypeInfo dtgtk_gradient_slider_info =
    {
      "GtkDarktableGradientSlider",
      sizeof(GtkDarktableGradientSlider),
      sizeof(GtkDarktableGradientSliderClass),
      (GtkClassInitFunc) _gradient_slider_class_init,
      (GtkObjectInitFunc) _gradient_slider_init,
      NULL,
      NULL,
      (GtkClassInitFunc) NULL
    };
    dtgtk_gradient_slider_type = gtk_type_unique(GTK_TYPE_WIDGET, &dtgtk_gradient_slider_info);
  }
  return dtgtk_gradient_slider_type;
}

gdouble dtgtk_gradient_slider_multivalue_get_value(GtkDarktableGradientSlider *gslider, gint pos)
{
  assert(pos <= gslider->positions);

  return gslider->position[pos];
}

void dtgtk_gradient_slider_multivalue_set_value(GtkDarktableGradientSlider *gslider,gdouble value, gint pos)
{
  assert(pos <= gslider->positions);

  gslider->position[pos]=value;
  g_signal_emit_by_name(G_OBJECT(gslider),"value-changed");
  gtk_widget_queue_draw(GTK_WIDGET(gslider));
}

void dtgtk_gradient_slider_multivalue_set_values(GtkDarktableGradientSlider *gslider,gdouble *values)
{
  for(int k=0; k<gslider->positions; k++) gslider->position[k] = values[k];
  g_signal_emit_by_name(G_OBJECT(gslider),"value-changed");
  gtk_widget_queue_draw(GTK_WIDGET(gslider));
}

void dtgtk_gradient_slider_multivalue_set_marker(GtkDarktableGradientSlider *gslider,gint mark, gint pos)
{
  assert(pos <= gslider->positions);

  gslider->marker[pos]=mark;
  gtk_widget_queue_draw(GTK_WIDGET(gslider));
}

void dtgtk_gradient_slider_multivalue_set_markers(GtkDarktableGradientSlider *gslider,gint *markers)
{
  for(int k=0; k<gslider->positions; k++) gslider->marker[k] = markers[k];
  gtk_widget_queue_draw(GTK_WIDGET(gslider));
}

void dtgtk_gradient_slider_multivalue_set_resetvalue(GtkDarktableGradientSlider *gslider,gdouble value, gint pos)
{
  assert(pos <= gslider->positions);

  gslider->resetvalue[pos]=value;
  gslider->is_resettable = TRUE;
}

void dtgtk_gradient_slider_multivalue_set_resetvalues(GtkDarktableGradientSlider *gslider,gdouble *values)
{
  for(int k=0; k<gslider->positions; k++) gslider->resetvalue[k] = values[k];
  gslider->is_resettable = TRUE;
}


gboolean dtgtk_gradient_slider_multivalue_is_dragging(GtkDarktableGradientSlider *gslider)
{
  return gslider->is_dragging;
}

// Public functions for single value type
GtkWidget* dtgtk_gradient_slider_new()
{
  return dtgtk_gradient_slider_multivalue_new(1);
}

GtkWidget* dtgtk_gradient_slider_new_with_color(GdkColor start,GdkColor end)
{
  return dtgtk_gradient_slider_multivalue_new_with_color(start, end, 1);
}

void dtgtk_gradient_slider_set_stop(GtkDarktableGradientSlider *gslider,gfloat position,GdkColor color)
{
  dtgtk_gradient_slider_multivalue_set_stop(gslider, position, color);
}

GtkType dtgtk_gradient_slider_get_type()
{
  return dtgtk_gradient_slider_multivalue_get_type();
}

gdouble dtgtk_gradient_slider_get_value(GtkDarktableGradientSlider *gslider)
{
  return dtgtk_gradient_slider_multivalue_get_value(gslider, 0);
}

void dtgtk_gradient_slider_multivalue_get_values(GtkDarktableGradientSlider *gslider, gdouble *values)
{
  for(int k=0; k<gslider->positions; k++) values[k] = gslider->position[k];
}

void dtgtk_gradient_slider_set_value(GtkDarktableGradientSlider *gslider,gdouble value)
{
  dtgtk_gradient_slider_multivalue_set_value(gslider, value, 0);
}

void dtgtk_gradient_slider_set_marker(GtkDarktableGradientSlider *gslider,gint mark)
{
  dtgtk_gradient_slider_multivalue_set_marker(gslider, mark, 0);
}

void dtgtk_gradient_slider_set_resetvalue(GtkDarktableGradientSlider *gslider,gdouble value)
{
  dtgtk_gradient_slider_multivalue_set_resetvalue(gslider, value, 0);
}

gboolean dtgtk_gradient_slider_is_dragging(GtkDarktableGradientSlider *gslider)
{
  return gslider->is_dragging;
}

