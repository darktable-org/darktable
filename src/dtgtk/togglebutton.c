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
static gboolean _togglebutton_draw(GtkWidget *widget, cairo_t *cr);

static void _togglebutton_class_init(GtkDarktableToggleButtonClass *klass)
{
  GtkWidgetClass *widget_class = (GtkWidgetClass *)klass;

  widget_class->draw = _togglebutton_draw;
}

static void _togglebutton_init(GtkDarktableToggleButton *slider)
{
}

static gboolean _togglebutton_draw(GtkWidget *widget, cairo_t *cr)
{
  g_return_val_if_fail(widget != NULL, FALSE);
  g_return_val_if_fail(DTGTK_IS_TOGGLEBUTTON(widget), FALSE);

  GtkDarktableToggleButton *button = DTGTK_TOGGLEBUTTON(widget);

  GtkStateFlags state = gtk_widget_get_state_flags(widget);

  GdkRGBA bg_color, fg_color;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  if(button->icon_flags & CPF_CUSTOM_BG)
    bg_color = button->bg;
  else
    gtk_style_context_get_background_color(context, state, &bg_color);
  if(button->icon_flags & CPF_CUSTOM_FG)
    fg_color = button->fg;
  else
    gtk_style_context_get_color(context, state, &fg_color);

  /* fetch flags */
  int flags = DTGTK_TOGGLEBUTTON(widget)->icon_flags;

  /* set inner border */
  int border = DT_PIXEL_APPLY_DPI((flags & CPF_DO_NOT_USE_BORDER) ? 2 : 6);

  /* update active state paint flag */
  gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(active)
    flags |= CPF_ACTIVE;
  else
    flags &= ~(CPF_ACTIVE);

  /* prelight */
  if(state & GTK_STATE_FLAG_PRELIGHT)
    flags |= CPF_PRELIGHT;
  else
    flags &= ~CPF_PRELIGHT;

  /* begin cairo drawing */
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width;
  int height = allocation.height;

  /* draw standard button background if not transparent nor flat styled */
  if((flags & CPF_STYLE_FLAT))
  {
    if(flags & CPF_PRELIGHT || flags & CPF_ACTIVE)
    {
      cairo_rectangle(cr, 0, 0, width, height);
      gdk_cairo_set_source_rgba(cr, &bg_color);
      cairo_fill(cr);
    }
  }
  else if(!(flags & CPF_BG_TRANSPARENT))
  {
    /* draw default boxed button */
    gtk_render_background(context, cr, 0, 0, width, height);
    if(!(flags & CPF_DO_NOT_USE_BORDER))
      gtk_render_frame(context, cr, 0, 0, width, height);
  }


  /* create pango text settings if label exists */
  PangoLayout *layout = NULL;
  int pw = 0, ph = 0;
  const gchar *text = gtk_button_get_label(GTK_BUTTON(widget));
  if(text)
  {
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, darktable.bauhaus->pango_font_desc);
    pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_size(layout, &pw, &ph);
  }

  gdk_cairo_set_source_rgba(cr, &fg_color);

  /* draw icon */
  if(DTGTK_TOGGLEBUTTON(widget)->icon)
  {
    //     if (flags & CPF_IGNORE_FG_STATE)
    //       state = GTK_STATE_NORMAL;

    int icon_width = text ? height - (border * 2) : width - (border * 2);
    int icon_height = height - (border * 2);

    if(icon_width > 0 && icon_height > 0)
    {
      if(text)
        DTGTK_TOGGLEBUTTON(widget)
            ->icon(cr, border, border, height - (border * 2), height - (border * 2), flags);
      else
        DTGTK_TOGGLEBUTTON(widget)
            ->icon(cr, border, border, width - (border * 2), height - (border * 2), flags);
    }
  }


  /* draw label */
  if(text)
  {
    int lx = DT_PIXEL_APPLY_DPI(2), ly = ((height / 2.0) - (ph / 2.0));
    // if (DTGTK_TOGGLEBUTTON (widget)->icon) lx += width;
    // GdkRectangle t={x,y,x+width,y+height};
    // gtk_paint_layout(style,gtk_widget_get_window(widget),
    // state,TRUE,&t,widget,"togglebutton",lx,ly,layout);
    cairo_translate(cr, lx, ly);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
  }

  return FALSE;
}

// Public functions
GtkWidget *dtgtk_togglebutton_new(DTGTKCairoPaintIconFunc paint, gint paintflags)
{
  GtkDarktableToggleButton *button;
  button = g_object_new(dtgtk_togglebutton_get_type(), NULL);
  button->icon = paint;
  button->icon_flags = paintflags;
  gtk_widget_set_size_request(GTK_WIDGET(button), DT_PIXEL_APPLY_DPI(17), DT_PIXEL_APPLY_DPI(17));
  return (GtkWidget *)button;
}

GType dtgtk_togglebutton_get_type()
{
  static GType dtgtk_togglebutton_type = 0;
  if(!dtgtk_togglebutton_type)
  {
    static const GTypeInfo dtgtk_togglebutton_info = {
      sizeof(GtkDarktableToggleButtonClass), (GBaseInitFunc)NULL, (GBaseFinalizeFunc)NULL,
      (GClassInitFunc)_togglebutton_class_init, NULL, /* class_finalize */
      NULL,                                           /* class_data */
      sizeof(GtkDarktableToggleButton), 0,            /* n_preallocs */
      (GInstanceInitFunc)_togglebutton_init,
    };
    dtgtk_togglebutton_type = g_type_register_static(GTK_TYPE_TOGGLE_BUTTON, "GtkDarktableToggleButton",
                                                     &dtgtk_togglebutton_info, 0);
  }
  return dtgtk_togglebutton_type;
}


void dtgtk_togglebutton_set_paint(GtkDarktableToggleButton *button, DTGTKCairoPaintIconFunc paint,
                                  gint paintflags)
{
  button->icon = paint;
  button->icon_flags = paintflags;
}

void dtgtk_togglebutton_override_color(GtkDarktableToggleButton *button, GdkRGBA *color)
{
  if(color)
  {
    button->fg = *color;
    button->icon_flags |= CPF_CUSTOM_FG;
  }
  else
    button->icon_flags &= ~CPF_CUSTOM_FG;
}

void dtgtk_togglebutton_override_background_color(GtkDarktableToggleButton *button, GdkRGBA *color)
{
  if(color)
  {
    button->bg = *color;
    button->icon_flags |= CPF_CUSTOM_BG;
  }
  else
    button->icon_flags &= ~CPF_CUSTOM_BG;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
