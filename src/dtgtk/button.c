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
#include "bauhaus/bauhaus.h"

static void _button_class_init(GtkDarktableButtonClass *klass);
static void _button_init(GtkDarktableButton *button);
static gboolean _button_draw(GtkWidget *widget, cairo_t *cr);

static void _button_class_init(GtkDarktableButtonClass *klass)
{
  GtkWidgetClass *widget_class = (GtkWidgetClass *)klass;

  widget_class->draw = _button_draw;
}

static void _button_init(GtkDarktableButton *button)
{
}

static gboolean _button_draw(GtkWidget *widget, cairo_t *cr)
{
  g_return_val_if_fail(widget != NULL, FALSE);
  g_return_val_if_fail(DTGTK_IS_BUTTON(widget), FALSE);

  GtkStateFlags state = gtk_widget_get_state_flags(widget);

  GdkRGBA bg_color, fg_color;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gtk_style_context_get_background_color(context, state, &bg_color);
  gtk_style_context_get_color(context, state, &fg_color);

  /* update paint flags depending of states */
  int flags = DTGTK_BUTTON(widget)->icon_flags;

  /* set inner border */
  int border = DT_PIXEL_APPLY_DPI((flags & CPF_DO_NOT_USE_BORDER) ? 2 : 6);

  /* prelight */
  if(state & GTK_STATE_FLAG_PRELIGHT)
    flags |= CPF_PRELIGHT;
  else
    flags &= ~CPF_PRELIGHT;


  /* create pango text settings if label exists */
  PangoLayout *layout = NULL;
  int pw = 0, ph = 0;
  const gchar *text = gtk_button_get_label(GTK_BUTTON(widget));
  if(text)
  {
    layout = gtk_widget_create_pango_layout(widget, NULL);
    pango_layout_set_font_description(layout, darktable.bauhaus->pango_font_desc);
    pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_size(layout, &pw, &ph);
  }

  /* begin cairo drawing */
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width;
  int height = allocation.height;

  /* draw standard button background if not transparent */
  if((flags & CPF_STYLE_FLAT))
  {
    if(flags & CPF_PRELIGHT)
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

  gdk_cairo_set_source_rgba(cr, &fg_color);

  /* draw icon */
  if(DTGTK_BUTTON(widget)->icon)
  {
    int icon_width = text ? height - (border * 2) : width - (border * 2);
    int icon_height = height - (border * 2);

    if(icon_width > 0 && icon_height > 0)
    {
      if(text)
        DTGTK_BUTTON(widget)
            ->icon(cr, border, border, height - (border * 2), height - (border * 2), flags);
      else
        DTGTK_BUTTON(widget)
            ->icon(cr, border, border, width - (border * 2), height - (border * 2), flags);
    }
  }

  /* draw label */
  if(text)
  {
    int lx = DT_PIXEL_APPLY_DPI(2), ly = ((height / 2.0) - (ph / 2.0));
    if(DTGTK_BUTTON(widget)->icon) lx += width;
    cairo_move_to(cr, lx, ly);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
  }

  return FALSE;
}

// Public functions
GtkWidget *dtgtk_button_new(DTGTKCairoPaintIconFunc paint, gint paintflags)
{
  GtkDarktableButton *button;
  button = g_object_new(dtgtk_button_get_type(), NULL);
  button->icon = paint;
  button->icon_flags = paintflags;
  gtk_widget_set_size_request(GTK_WIDGET(button), DT_PIXEL_APPLY_DPI(17), DT_PIXEL_APPLY_DPI(17));
  return (GtkWidget *)button;
}

GType dtgtk_button_get_type()
{
  static GType dtgtk_button_type = 0;
  if(!dtgtk_button_type)
  {
    static const GTypeInfo dtgtk_button_info = {
      sizeof(GtkDarktableButtonClass), (GBaseInitFunc)NULL, (GBaseFinalizeFunc)NULL,
      (GClassInitFunc)_button_class_init, NULL, /* class_finalize */
      NULL,                                     /* class_data */
      sizeof(GtkDarktableButton), 0,            /* n_preallocs */
      (GInstanceInitFunc)_button_init,
    };
    dtgtk_button_type = g_type_register_static(GTK_TYPE_BUTTON, "GtkDarktableButton", &dtgtk_button_info, 0);
  }
  return dtgtk_button_type;
}

void dtgtk_button_set_paint(GtkDarktableButton *button, DTGTKCairoPaintIconFunc paint, gint paintflags)
{
  button->icon = paint;
  button->icon_flags = paintflags;
}

void dtgtk_button_override_color(GtkDarktableButton *button, GdkRGBA *color)
{
  if(color)
  {
    button->fg = *color;
    button->icon_flags |= CPF_CUSTOM_FG;
  }
  else
    button->icon_flags &= ~CPF_CUSTOM_FG;
}

void dtgtk_button_override_background_color(GtkDarktableButton *button, GdkRGBA *color)
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
