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
#include "button.h"
#include "gui/gtk.h"

static void _button_class_init (GtkDarktableButtonClass *klass);
static void _button_init (GtkDarktableButton *button);
static void _button_size_request (GtkWidget *widget, GtkRequisition *requisition);
static gboolean _button_expose (GtkWidget *widget, GdkEventExpose *event);


static void
_button_class_init (GtkDarktableButtonClass *klass)
{
  GtkWidgetClass *widget_class=(GtkWidgetClass *) klass;
  widget_class->size_request = _button_size_request;
  widget_class->expose_event = _button_expose;
}

static void
_button_init(GtkDarktableButton *button)
{
}

static void
_button_size_request(GtkWidget *widget,GtkRequisition *requisition)
{
  g_return_if_fail (widget != NULL);
  g_return_if_fail (DTGTK_IS_BUTTON(widget));
  g_return_if_fail (requisition != NULL);
  requisition->width = 17;
  requisition->height = 17;
}

static gboolean
_button_expose (GtkWidget *widget, GdkEventExpose *event)
{
  g_return_val_if_fail (widget != NULL, FALSE);
  g_return_val_if_fail (DTGTK_IS_BUTTON(widget), FALSE);
  g_return_val_if_fail (event != NULL, FALSE);
  GtkStyle *style=gtk_widget_get_style(widget);
  int state = gtk_widget_get_state(widget);

  /* update paint flags depending of states */
  int flags = DTGTK_BUTTON (widget)->icon_flags;

  /* set inner border */
  int border = (flags&CPF_DO_NOT_USE_BORDER)?2:4;

  /* prelight */
  if (state == GTK_STATE_PRELIGHT)
    flags |= CPF_PRELIGHT;
  else
    flags &=~CPF_PRELIGHT;


  /* create pango text settings if label exists */
  PangoLayout *layout=NULL;
  int pw=0,ph=0;
  const gchar *text=gtk_button_get_label (GTK_BUTTON (widget));
  if (text)
  {
    layout = gtk_widget_create_pango_layout (widget,NULL);
    pango_layout_set_font_description (layout,style->font_desc);
    pango_layout_set_text (layout,text,-1);
    pango_layout_get_pixel_size (layout,&pw,&ph);
  }

  /* begin cairo drawing */
  cairo_t *cr;
  cr = gdk_cairo_create (widget->window);

  int x = widget->allocation.x;
  int y = widget->allocation.y;
  int width = widget->allocation.width;
  int height = widget->allocation.height;

  /* draw standard button background if not transparent */
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
    /* draw default boxed button */
    gtk_paint_box (widget->style, widget->window,
                   GTK_WIDGET_STATE (widget),
                   GTK_SHADOW_OUT, NULL, widget, "button",
                   x, y, width, height);
  }

  if (flags & CPF_IGNORE_FG_STATE)
    state = GTK_STATE_NORMAL;

  cairo_set_source_rgb (cr,
                        style->fg[state].red/65535.0,
                        style->fg[state].green/65535.0,
                        style->fg[state].blue/65535.0);

  /* draw icon */
  if (DTGTK_BUTTON (widget)->icon)
  {
    if (text)
      DTGTK_BUTTON (widget)->icon (cr,x+border,y+border,height-(border*2),height-(border*2),flags);
    else
      DTGTK_BUTTON (widget)->icon (cr,x+border,y+border,width-(border*2),height-(border*2),flags);
  }
  cairo_destroy (cr);

  /* draw label */
  if (text)
  {
    int lx=x+2, ly=y+((height/2.0)-(ph/2.0));
    if (DTGTK_BUTTON (widget)->icon) lx += width;
    GdkRectangle t= {x,y,x+width,y+height};
    gtk_paint_layout(style,widget->window, GTK_STATE_NORMAL,TRUE,&t,widget,"label",lx,ly,layout);
  }

  return FALSE;
}

// Public functions
GtkWidget*
dtgtk_button_new (DTGTKCairoPaintIconFunc paint, gint paintflags)
{
  GtkDarktableButton *button;
  button = gtk_type_new (dtgtk_button_get_type());
  button->icon = paint;
  button->icon_flags = paintflags;
  return (GtkWidget *)button;
}

GtkWidget*
dtgtk_button_new_with_label (const gchar *label, DTGTKCairoPaintIconFunc paint, gint paintflags)
{
  GtkWidget *button = dtgtk_button_new (paint,paintflags);

  /* set button label */
  gtk_button_set_label (GTK_BUTTON (button),label);

  return button;
}

GtkType dtgtk_button_get_type()
{
  static GtkType dtgtk_button_type = 0;
  if (!dtgtk_button_type)
  {
    static const GtkTypeInfo dtgtk_button_info =
    {
      "GtkDarktableButton",
      sizeof(GtkDarktableButton),
      sizeof(GtkDarktableButtonClass),
      (GtkClassInitFunc) _button_class_init,
      (GtkObjectInitFunc) _button_init,
      NULL,
      NULL,
      (GtkClassInitFunc) NULL
    };
    dtgtk_button_type = gtk_type_unique (GTK_TYPE_BUTTON, &dtgtk_button_info);
  }
  return dtgtk_button_type;
}

void dtgtk_button_set_paint(GtkDarktableButton *button,
                            DTGTKCairoPaintIconFunc paint,
                            gint paintflags)
{
  button->icon = paint;
  button->icon_flags = paintflags;
}

// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
