/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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
#include "tristatebutton.h"
#include "button.h"

static guint _tristatebutton_signals[TRISTATEBUTTON_LAST_SIGNAL] = { 0 };

static void _tristatebutton_class_init(GtkDarktableTriStateButtonClass *klass);
static void _tristatebutton_init(GtkDarktableTriStateButton *slider);
static void _tristatebutton_size_request(GtkWidget *widget, GtkRequisition *requisition);
//static void _tristatebutton_size_allocate(GtkWidget *widget, GtkAllocation *allocation);
//static void _tristatebutton_realize(GtkWidget *widget);
static gboolean _tristatebutton_expose(GtkWidget *widget, GdkEventExpose *event);
//static void _tristatebutton_destroy(GtkObject *object);

static void _tristate_emit_state_changed_signal(GtkDarktableTriStateButton *ts)
{
  g_signal_emit (ts,
                 _tristatebutton_signals[STATE_CHANGED],
                 0,
                 ts->state);
}

static void _tristatebutton_class_init (GtkDarktableTriStateButtonClass *klass)
{
  GtkWidgetClass *widget_class=(GtkWidgetClass *) klass;
  //GtkObjectClass *object_class=(GtkObjectClass *) klass;
  //widget_class->realize = _tristatebutton_realize;
  widget_class->size_request = _tristatebutton_size_request;
  //widget_class->size_allocate = _tristatebutton_size_allocate;
  widget_class->expose_event = _tristatebutton_expose;
  //object_class->destroy = _tristatebutton_destroy;

  _tristatebutton_signals[STATE_CHANGED] = g_signal_new (
        "tristate-changed",
        G_OBJECT_CLASS_TYPE (klass),
        G_SIGNAL_RUN_FIRST,
        G_STRUCT_OFFSET (GtkDarktableTriStateButtonClass, state_changed),
        NULL, NULL,
        g_cclosure_marshal_VOID__INT,
        G_TYPE_NONE, 1,
        G_TYPE_INT);

}

static void _tristatebutton_init(GtkDarktableTriStateButton *slider)
{
}

static void  _tristatebutton_size_request(GtkWidget *widget,GtkRequisition *requisition)
{
  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_TRISTATEBUTTON(widget));
  g_return_if_fail(requisition != NULL);

  /* create pango text settings if label exists */
  GtkStyle *style = gtk_widget_get_style(widget);
  PangoLayout *layout = NULL;
  int pw=0,ph=0;
  const gchar *text=gtk_button_get_label (GTK_BUTTON (widget));
  if (text)
  {
    layout = gtk_widget_create_pango_layout (widget,NULL);
    pango_layout_set_font_description (layout,style->font_desc);
    pango_layout_set_text (layout,text,-1);
    pango_layout_get_pixel_size (layout,&pw,&ph);

    requisition->width = pw+4;
    requisition->height = ph+4;
  }
  else
  {
    requisition->width = requisition->height = 24;
  }
}

#if 0
static void _tristatebutton_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_TRISTATEBUTTON(widget));
  g_return_if_fail(allocation != NULL);

  widget->allocation = *allocation;

  if (GTK_WIDGET_REALIZED(widget))
  {
    gdk_window_move_resize(
      widget->window,
      allocation->x, allocation->y,
      allocation->width, allocation->height
    );
  }
}

static void _tristatebutton_destroy(GtkObject *object)
{
  GtkDarktableTriStateButtonClass *klass;
  g_return_if_fail(object != NULL);
  g_return_if_fail(DTGTK_IS_TRISTATEBUTTON(object));
  klass = gtk_type_class(gtk_widget_get_type());
  if (GTK_OBJECT_CLASS(klass)->destroy)
  {
    (* GTK_OBJECT_CLASS(klass)->destroy) (object);
  }
}
#endif

static gboolean _tristatebutton_expose(GtkWidget *widget, GdkEventExpose *event)
{
  g_return_val_if_fail (widget != NULL, FALSE);
  g_return_val_if_fail (DTGTK_IS_TRISTATEBUTTON(widget), FALSE);
  g_return_val_if_fail (event != NULL, FALSE);
  GtkStyle *style=gtk_widget_get_style(widget);
  int state = gtk_widget_get_state(widget);

  /* fix text style */
  for(int i=0; i<5; i++)
    style->text[i]=style->fg[i];

  /* fetch flags */
  int flags = DTGTK_TRISTATEBUTTON (widget)->icon_flags;

  /* set inner border */
  int border = (flags&CPF_DO_NOT_USE_BORDER)?2:6;

  /* update active state paint flag */
  gboolean active = DTGTK_TRISTATEBUTTON(widget)->state>0?TRUE:FALSE;
  if (active)
    flags |= CPF_ACTIVE;
  else
    flags &=~(CPF_ACTIVE);


  /* begin cairo drawing */
  cairo_t *cr;
  cr = gdk_cairo_create (widget->window);

  int x = widget->allocation.x;
  int y = widget->allocation.y;
  int width = widget->allocation.width;
  int height = widget->allocation.height;

  /* draw standard button background if not transparent nor flat styled */
  if( (flags & CPF_STYLE_FLAT ))
  {
    if( state != GTK_STATE_NORMAL )
    {
      cairo_rectangle (cr,x,y,width,height);
      cairo_set_source_rgba (cr,
                             style->bg[state].red/65535.0,
                             style->bg[state].green/65535.0,
                             style->bg[state].blue/65535.0,
                             0.5);
      cairo_fill (cr);
    }
  }
  else if( !(flags & CPF_BG_TRANSPARENT) )
  {
    cairo_rectangle (cr,x,y,width,height);
    float rs=1.0,gs=1.0,bs=1.0;

    if(DTGTK_TRISTATEBUTTON(widget)->state == 1)
      rs=gs=bs=3.0;
    else if(DTGTK_TRISTATEBUTTON(widget)->state == 2)
      rs=3.0;

    cairo_set_source_rgba (cr,
                           (style->bg[state].red/65535.0)*rs,
                           (style->bg[state].green/65535.0)*gs,
                           (style->bg[state].blue/65535.0)*bs,
                           0.5);
    cairo_fill (cr);
  }

  /* create pango text settings if label exists */
  PangoLayout *layout=NULL;
  int pw=0,ph=0;
  const gchar *text=gtk_button_get_label (GTK_BUTTON (widget));
  if (text)
  {
    layout = pango_cairo_create_layout (cr);
    pango_layout_set_font_description (layout,style->font_desc);
    pango_layout_set_text (layout,text,-1);
    pango_layout_get_pixel_size (layout,&pw,&ph);
  }

  cairo_set_source_rgb (cr,
                        style->fg[state].red/65535.0,
                        style->fg[state].green/65535.0,
                        style->fg[state].blue/65535.0);

  /* draw button image if any */
  GtkWidget *image=gtk_button_get_image(GTK_BUTTON(widget));
  if(image)
  {
    GdkPixbuf *pixbuf = gtk_image_get_pixbuf(GTK_IMAGE(image));

    if(pixbuf)
    {
      /* Draw the pixbuf */
      gint pbw = gdk_pixbuf_get_width (pixbuf);
      gint pbh = gdk_pixbuf_get_height (pixbuf);
      gdk_cairo_set_source_pixbuf (cr, pixbuf, widget->allocation.x+((widget->allocation.width/2)-(pbw/2)),
                                  widget->allocation.y+((widget->allocation.height/2)-(pbh/2)));
      cairo_paint (cr);
    }
  }


  /* draw icon */
  if (DTGTK_TRISTATEBUTTON (widget)->icon)
  {
//     if (flags & CPF_IGNORE_FG_STATE)
//       state = GTK_STATE_NORMAL;


    if (text)
      DTGTK_TRISTATEBUTTON (widget)->icon (cr,x+border,y+border,height-(border*2),height-(border*2),flags);
    else
      DTGTK_TRISTATEBUTTON (widget)->icon (cr,x+border,y+border,width-(border*2),height-(border*2),flags);
  }


  /* draw label */
  if (text)
  {
    int lx=x+2, ly=y+((height/2.0)-(ph/2.0));
    cairo_translate(cr, lx, ly);
    pango_cairo_show_layout (cr,layout);
  }

  cairo_destroy (cr);

  return FALSE;
}

static gboolean _tristatebutton_button_press(GtkWidget *widget,  GdkEventButton *eb, gpointer data)
{
  gint cs = DTGTK_TRISTATEBUTTON(widget)->state + 1;
  /* handle left click on tristate button */
  if(eb->button == 1)
    cs %= 3;
  /* handle right click on tristate button */
  else if(eb->button == 2)
    cs = 0;

  dtgtk_tristatebutton_set_state(DTGTK_TRISTATEBUTTON(widget), cs);
  gtk_widget_queue_draw(widget);

  /* lets other connected get the signal... */
  return FALSE;
}

// Public functions
GtkWidget*
dtgtk_tristatebutton_new (DTGTKCairoPaintIconFunc paint, gint paintflags)
{
  GtkDarktableTriStateButton *button;
  button = gtk_type_new(dtgtk_tristatebutton_get_type());
  button->icon=paint;
  button->icon_flags=paintflags;
  g_signal_connect(G_OBJECT(button), "button-press-event",
                   G_CALLBACK(_tristatebutton_button_press), NULL);

  return (GtkWidget *)button;
}


GtkWidget*
dtgtk_tristatebutton_new_with_label (const gchar *label, DTGTKCairoPaintIconFunc paint, gint paintflags)
{
  GtkWidget *button = dtgtk_tristatebutton_new(paint,paintflags);
  gtk_button_set_label(GTK_BUTTON (button), label);
  return button;
}

void dtgtk_tristatebutton_set_state(GtkDarktableTriStateButton *ts, gint state)
{
  ts->state = fmax(0,fmin(3,state));
  _tristate_emit_state_changed_signal(ts);
}

gint dtgtk_tristatebutton_get_state(const GtkDarktableTriStateButton *ts)
{
  return ts->state;
}

GtkType dtgtk_tristatebutton_get_type()
{
  static GtkType dtgtk_tristatebutton_type = 0;
  if (!dtgtk_tristatebutton_type)
  {
    static const GtkTypeInfo dtgtk_tristatebutton_info =
    {
      "GtkDarktableTriStateButton",
      sizeof(GtkDarktableTriStateButton),
      sizeof(GtkDarktableTriStateButtonClass),
      (GtkClassInitFunc) _tristatebutton_class_init,
      (GtkObjectInitFunc) _tristatebutton_init,
      NULL,
      NULL,
      (GtkClassInitFunc) NULL
    };
    dtgtk_tristatebutton_type = gtk_type_unique(GTK_TYPE_BUTTON, &dtgtk_tristatebutton_info);
  }
  return dtgtk_tristatebutton_type;
}

// These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
