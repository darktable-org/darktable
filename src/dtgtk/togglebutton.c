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
#include "togglebutton.h"
#include "bauhaus/bauhaus.h"
#include "button.h"
#include "gui/gtk.h"
#include <string.h>

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

  GdkRGBA fg_color;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  if(button->icon_flags & CPF_CUSTOM_FG)
    fg_color = button->fg;
  else if(button->icon_flags & CPF_IGNORE_FG_STATE)
    gtk_style_context_get_color(context, state & ~GTK_STATE_FLAG_SELECTED, &fg_color);
  else
    gtk_style_context_get_color(context, state, &fg_color);

  /* fetch flags */
  int flags = DTGTK_TOGGLEBUTTON(widget)->icon_flags;

  /* update active state paint flag */
  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(active)
    flags |= CPF_ACTIVE;
  else
    flags &= ~CPF_ACTIVE;

  /* update focus state paint flag */
  const gboolean hasfocus = ((DTGTK_TOGGLEBUTTON(widget)->icon_data == darktable.develop->gui_module)
                         && darktable.develop->gui_module);
  if(hasfocus)
    flags |= CPF_FOCUS;
  else
    flags &= ~CPF_FOCUS;

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

  /* draw standard button background if not transparent nor flat styled */
  if((flags & CPF_STYLE_FLAT))
  {
    if((flags & CPF_PRELIGHT) || ((flags & CPF_ACTIVE) && !(flags & CPF_BG_TRANSPARENT)))
    {
      // When CPF_BG_TRANSPARENT is set, change the background on
      // PRELIGHT, but not on ACTIVE
      if(!(flags & CPF_BG_TRANSPARENT) || (flags & CPF_PRELIGHT))
        gtk_render_background(context, cr, startx, starty, cwidth, cheight);
    }
    if(!(flags & CPF_ACTIVE) || (flags & CPF_IGNORE_FG_STATE))
      fg_color.alpha = CLAMP(fg_color.alpha / 2.0, 0.3, 1.0);
  }
  else if(!(flags & CPF_BG_TRANSPARENT))
    gtk_render_background(context, cr, startx, starty, cwidth, cheight);

  gtk_render_frame(context, cr, startx, starty, cwidth, cheight);

  gdk_cairo_set_source_rgba(cr, &fg_color);

  /* draw icon */
  if(DTGTK_TOGGLEBUTTON(widget)->icon)
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
    GtkStyleContext *ccontext = gtk_widget_get_style_context(DTGTK_TOGGLEBUTTON(widget)->canvas);
    GtkBorder cmargin;
    gtk_style_context_get_margin(ccontext, state, &cmargin);

    startx += round(cmargin.left * cwidth / 100.0f);
    starty += round(cmargin.top * cheight / 100.0f);
    cwidth = round((float)cwidth * (1.0 - (cmargin.left + cmargin.right) / 100.0f));
    cheight = round((float)cheight * (1.0 - (cmargin.top + cmargin.bottom) / 100.0f));

    void *icon_data = DTGTK_TOGGLEBUTTON(widget)->icon_data;

    if(cwidth > 0 && cheight > 0)
      DTGTK_TOGGLEBUTTON(widget)->icon(cr, startx, starty, cwidth, cheight, flags, icon_data);
  }

  return FALSE;
}

// Public functions
GtkWidget *dtgtk_togglebutton_new(DTGTKCairoPaintIconFunc paint, gint paintflags, void *paintdata)
{
  GtkDarktableToggleButton *button;
  button = g_object_new(dtgtk_togglebutton_get_type(), NULL);
  button->icon = paint;
  button->icon_flags = paintflags;
  button->icon_data = paintdata;
  button->canvas = gtk_drawing_area_new();
  gtk_container_add(GTK_CONTAINER(button), button->canvas);
  gtk_widget_set_name(GTK_WIDGET(button), "dt-toggle-button");
  gtk_widget_set_name(GTK_WIDGET(button->canvas), "button-canvas");
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
                                  gint paintflags, void *paintdata)
{
  g_return_if_fail(button != NULL);
  button->icon = paint;
  button->icon_flags = paintflags;
  button->icon_data = paintdata;
}

void dtgtk_togglebutton_override_color(GtkDarktableToggleButton *button, GdkRGBA *color)
{
  g_return_if_fail(button != NULL);
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
  g_return_if_fail(button != NULL);
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
