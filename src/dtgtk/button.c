/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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
  /* get button total allocation */
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int width = allocation.width;
  const int height = allocation.height;

  /* get the css geometry properties of the button */
  GtkBorder margin, border, padding;
  gtk_style_context_get_margin(context, state, &margin);
  gtk_style_context_get_border(context, state, &border);
  gtk_style_context_get_padding(context, state, &padding);

  /* for button frame and background, we remove css margin from allocation */
  int startx = margin.left;
  int starty = margin.top;
  int cwidth = width - margin.left - margin.right;
  int cheight = height - margin.top - margin.bottom;

  /* draw standard button background and borders */
  gtk_render_background(context, cr, startx, starty, cwidth, cheight);
  gtk_render_frame(context, cr, startx, starty, cwidth, cheight);
  gdk_cairo_set_source_rgba(cr, &fg_color);

  /* draw icon */
  if(DTGTK_BUTTON(widget)->icon)
  {
    /* calculate the button content allocation */
    startx += border.left + padding.left;
    starty += border.top + padding.top;
    cwidth -= border.left + border.right + padding.left + padding.right;
    cheight -= border.top + border.bottom + padding.top + padding.bottom;

    /* we have to leave some breathing room to the cairo icon paint function to possibly    */
    /* draw slightly outside the bounding box, for optical alignment and balancing of icons */
    /* we do this by putting a drawing area widget inside the button and using the CSS      */
    /* margin property in px of the drawing area as extra room in percent (DPI safe)        */
    /* we do this because Gtk+ does not support CSS size in percent                         */
    /* this extra margin can be also (slightly) negative                                    */
    GtkStyleContext *ccontext = gtk_widget_get_style_context(DTGTK_BUTTON(widget)->canvas);
    GtkBorder cmargin;
    gtk_style_context_get_margin(ccontext, state, &cmargin);

    startx += round(cmargin.left * cwidth / 100.0f);
    starty += round(cmargin.top * cheight / 100.0f);
    cwidth = round((float)cwidth * (1.0 - (cmargin.left + cmargin.right) / 100.0f));
    cheight = round((float)cheight * (1.0 - (cmargin.top + cmargin.bottom) / 100.0f));

    void *icon_data = DTGTK_BUTTON(widget)->icon_data;
    if(cwidth > 0 && cheight > 0)
      DTGTK_BUTTON(widget)->icon(cr, startx, starty, cwidth, cheight, flags, icon_data);
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
  button->canvas = gtk_drawing_area_new();
  gtk_container_add(GTK_CONTAINER(button), button->canvas);
  dt_gui_add_class(GTK_WIDGET(button), "dt_module_btn");
  gtk_widget_set_name(GTK_WIDGET(button->canvas), "button-canvas");
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
  g_return_if_fail(button != NULL);
  button->icon = paint;
  button->icon_flags = paintflags;
  button->icon_data = paintdata;
}

void dtgtk_button_set_active(GtkDarktableButton *button, gboolean active)
{
  g_return_if_fail(button != NULL);
  if(active)
    button->icon_flags |= CPF_ACTIVE;
  else
    button->icon_flags &= ~CPF_ACTIVE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
