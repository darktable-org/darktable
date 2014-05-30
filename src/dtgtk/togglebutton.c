/*
    This file is part of darktable,
    copyright (c) 2010 Henrik Andersson.

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
#include "togglebutton.h"
#include "button.h"
#include "gui/gtk.h"
#include "bauhaus/bauhaus.h"

static void _togglebutton_class_init(GtkDarktableToggleButtonClass *klass);
static void _togglebutton_init(GtkDarktableToggleButton *slider);
static void _togglebutton_size_request(GtkWidget *widget, GtkRequisition *requisition);
static void _togglebutton_size_allocate(GtkWidget *widget, GtkAllocation *allocation);
//static void _togglebutton_realize(GtkWidget *widget);
static gboolean _togglebutton_expose(GtkWidget *widget, GdkEventExpose *event);
static void _togglebutton_destroy(GtkObject *object);

void temp()
{
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

  /* create pango text settings if label exists */
  PangoLayout *layout = NULL;
  int pw=0,ph=0;
  const gchar *text=gtk_button_get_label (GTK_BUTTON (widget));
  if (text)
  {
    layout = gtk_widget_create_pango_layout (widget,NULL);
    pango_layout_set_font_description(layout, darktable.bauhaus->pango_font_desc);
    pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
    pango_layout_set_text (layout,text,-1);
    pango_layout_get_pixel_size (layout,&pw,&ph);
    g_object_unref(layout);

    requisition->width = pw + DT_PIXEL_APPLY_DPI(4);
    requisition->height = ph + DT_PIXEL_APPLY_DPI(4);
  }
  else
  {
    requisition->width = DT_PIXEL_APPLY_DPI(22);
    requisition->height = DT_PIXEL_APPLY_DPI(17);
  }
}

static void _togglebutton_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
  g_return_if_fail(widget != NULL);
  g_return_if_fail(DTGTK_IS_TOGGLEBUTTON(widget));
  g_return_if_fail(allocation != NULL);

  gtk_widget_get_allocation(widget, allocation);

  if (gtk_widget_get_realized(widget))
  {
    gdk_window_move_resize(
      gtk_widget_get_window(widget),
      allocation->x, allocation->y,
      allocation->width, allocation->height
    );
  }
}

static void _togglebutton_destroy(GtkObject *object)
{
  GtkDarktableToggleButtonClass *klass;
  g_return_if_fail(object != NULL);
  g_return_if_fail(DTGTK_IS_TOGGLEBUTTON(object));

  //FIXME: or it should be g_type_class_ref () ?
  klass = g_type_class_peek(gtk_widget_get_type());
  if (GTK_OBJECT_CLASS(klass)->destroy)
  {
    (* GTK_OBJECT_CLASS(klass)->destroy) (object);
  }
}

static gboolean _togglebutton_expose(GtkWidget *widget, GdkEventExpose *event)
{
  g_return_val_if_fail (widget != NULL, FALSE);
  g_return_val_if_fail (DTGTK_IS_TOGGLEBUTTON(widget), FALSE);
  g_return_val_if_fail (event != NULL, FALSE);
  GtkStyle *style=gtk_widget_get_style(widget);
  int state = gtk_widget_get_state(widget);

  /* fix text style */
  for(int i=0; i<5; i++)
    style->text[i]=style->fg[i];

  /* fetch flags */
  int flags = DTGTK_TOGGLEBUTTON (widget)->icon_flags;

  /* set inner border */
  int border = DT_PIXEL_APPLY_DPI((flags&CPF_DO_NOT_USE_BORDER)?2:6);

  /* update active state paint flag */
  gboolean active = gtk_toggle_button_get_active ( GTK_TOGGLE_BUTTON (widget));
  if (active)
    flags |= CPF_ACTIVE;
  else
    flags &=~(CPF_ACTIVE);

  /* prelight */
  if (state == GTK_STATE_PRELIGHT)
    flags |= CPF_PRELIGHT;
  else
    flags &=~CPF_PRELIGHT;

  /* begin cairo drawing */
  cairo_t *cr;
  cr = gdk_cairo_create (gtk_widget_get_window(widget));

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int x = allocation.x;
  int y = allocation.y;
  int width = allocation.width;
  int height = allocation.height;

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
    /* draw default boxed button */
    gtk_paint_box (gtk_widget_get_style(widget), gtk_widget_get_window(widget),
                   gtk_widget_get_state(widget),
                   GTK_SHADOW_OUT, NULL, widget, "button",
                   x, y, width, height);
  }


  /* create pango text settings if label exists */
  PangoLayout *layout=NULL;
  int pw=0,ph=0;
  const gchar *text=gtk_button_get_label (GTK_BUTTON (widget));
  if (text)
  {
    layout = pango_cairo_create_layout (cr);
    pango_layout_set_font_description(layout, darktable.bauhaus->pango_font_desc);
    pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
    pango_layout_set_text (layout,text,-1);
    pango_layout_get_pixel_size (layout,&pw,&ph);
  }

  cairo_set_source_rgb (cr,
                        style->fg[state].red/65535.0,
                        style->fg[state].green/65535.0,
                        style->fg[state].blue/65535.0);

  /* draw icon */
  if (DTGTK_TOGGLEBUTTON (widget)->icon)
  {
//     if (flags & CPF_IGNORE_FG_STATE)
//       state = GTK_STATE_NORMAL;


    if (text)
      DTGTK_TOGGLEBUTTON (widget)->icon (cr,x+border,y+border,height-(border*2),height-(border*2),flags);
    else
      DTGTK_TOGGLEBUTTON (widget)->icon (cr,x+border,y+border,width-(border*2),height-(border*2),flags);
  }


  /* draw label */
  if (text)
  {
    int lx=x+DT_PIXEL_APPLY_DPI(2), ly=y+((height/2.0)-(ph/2.0));
    //if (DTGTK_TOGGLEBUTTON (widget)->icon) lx += width;
    //GdkRectangle t={x,y,x+width,y+height};
    //gtk_paint_layout(style,gtk_widget_get_window(widget), state,TRUE,&t,widget,"togglebutton",lx,ly,layout);
    cairo_translate(cr, lx, ly);
    pango_cairo_show_layout (cr,layout);
    g_object_unref(layout);
  }

  cairo_destroy (cr);

  return FALSE;
}

// Public functions
GtkWidget*
dtgtk_togglebutton_new (DTGTKCairoPaintIconFunc paint, gint paintflags)
{
  GtkDarktableToggleButton *button;
  button = g_object_new(dtgtk_togglebutton_get_type(), NULL);
  button->icon=paint;
  button->icon_flags=paintflags;
  return (GtkWidget *)button;
}


GtkWidget*
dtgtk_togglebutton_new_with_label (const gchar *label, DTGTKCairoPaintIconFunc paint, gint paintflags)
{
  GtkWidget *button = dtgtk_togglebutton_new(paint,paintflags);
  gtk_button_set_label(GTK_BUTTON (button), label);
  return button;
}

GType dtgtk_togglebutton_get_type()
{
  static GType dtgtk_togglebutton_type = 0;
  if (!dtgtk_togglebutton_type)
  {
    static const GTypeInfo dtgtk_togglebutton_info =
    {
      sizeof(GtkDarktableToggleButtonClass),
      (GBaseInitFunc) NULL,
      (GBaseFinalizeFunc) NULL,
      (GClassInitFunc) _togglebutton_class_init,
      NULL,           /* class_finalize */
      NULL,           /* class_data */
      sizeof(GtkDarktableToggleButton),
      0,              /* n_preallocs */
      (GInstanceInitFunc) _togglebutton_init,
    };
    dtgtk_togglebutton_type = g_type_register_static(GTK_TYPE_TOGGLE_BUTTON, "GtkDarktableToggleButton", &dtgtk_togglebutton_info, 0);
  }
  return dtgtk_togglebutton_type;
}


void dtgtk_togglebutton_set_paint(GtkDarktableToggleButton *button,
                                  DTGTKCairoPaintIconFunc paint,
                                  gint paintflags)
{
  button->icon = paint;
  button->icon_flags = paintflags;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
