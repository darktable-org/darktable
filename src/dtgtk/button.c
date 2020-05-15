/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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
#include "bauhaus/bauhaus.h"
#include "gui/gtk.h"
#include <string.h>

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

  GdkRGBA fg_color;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gtk_style_context_get_color(context, state, &fg_color);

  /* update paint flags depending of states */
  int flags = DTGTK_BUTTON(widget)->icon_flags;

  /* prelight */
  if(state & GTK_STATE_FLAG_PRELIGHT)
    flags |= CPF_PRELIGHT;
  else
    flags &= ~CPF_PRELIGHT;

  /* begin cairo drawing */
  GtkAllocation allocation;
  GtkBorder padding;

  gtk_widget_get_allocation(widget, &allocation);
  gtk_style_context_get_padding(context, state, &padding);

  int width = allocation.width;
  int height = allocation.height;

  /* draw standard button background if not transparent */
  if(flags & CPF_STYLE_FLAT)
  {
    if(flags & CPF_PRELIGHT)
    {
      gtk_render_background(context, cr, 0, 0, width, height);
    }
    else if (!(flags & CPF_ACTIVE))
    {
      fg_color.alpha = CLAMP(fg_color.alpha / 2.0, 0.3, 1.0);
    }
  }
  else if(!(flags & CPF_BG_TRANSPARENT))
  {
    /* draw default boxed button */
    gtk_render_background(context, cr, 0, 0, width, height);
    if(!(FALSE))
      gtk_render_frame(context, cr, 0, 0, width, height);
  }

  gdk_cairo_set_source_rgba(cr, &fg_color);

  /* draw icon */
  if(DTGTK_BUTTON(widget)->icon)
  {
    /* set icon size and corresponding border */
    GtkAllocation clip_area;
    gtk_widget_get_clip(widget, &clip_area); // get the clip area, the icon drawing cannot exceed that
    int cwidth = clip_area.width;
    int cheight = clip_area.height;

    float borderx, bordery;
    int icon_width, icon_height;
    if(FALSE)
    {
      borderx = (width - cwidth) / 2.0;
      bordery = (height - cheight) / 2.0;
      icon_width = cwidth;
      icon_height = cheight;
    }
    else
    {
      borderx = round(fmaxf((width - cwidth) / 2.0, padding.left));
      bordery = round(fmaxf((height - cheight) / 2.0, padding.top));
      icon_width = round(width - 2.0 * borderx);
      icon_height = round(height - 2.0 * bordery);
    }
    void *icon_data = DTGTK_BUTTON(widget)->icon_data;
    if(icon_width > 0 && icon_height > 0)
      DTGTK_BUTTON(widget)->icon(cr, round(borderx), round(bordery), icon_width, icon_height, flags, icon_data);
  }

  return FALSE;
}

// Public functions
GtkWidget *dtgtk_button_new(DTGTKCairoPaintIconFunc paint, gint paintflags, void *paintdata)
{
  GtkDarktableButton *button;
  button = g_object_new(dtgtk_button_get_type(), NULL);
  button->icon = paint;
  button->icon_flags = paintflags;
  button->icon_data = paintdata;
  gtk_widget_set_name(GTK_WIDGET(button), "dt-button");
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

void dtgtk_button_set_paint(GtkDarktableButton *button, DTGTKCairoPaintIconFunc paint, gint paintflags, void *paintdata)
{
  button->icon = paint;
  button->icon_flags = paintflags;
  button->icon_data = paintdata;
}

void dtgtk_button_set_active(GtkDarktableButton *button, gboolean active)
{
  if(active)
    button->icon_flags |= CPF_ACTIVE;
  else
    button->icon_flags &= ~CPF_ACTIVE;
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
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
