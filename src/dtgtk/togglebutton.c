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

  GdkRGBA bg_color, fg_color;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  if(button->icon_flags & CPF_CUSTOM_BG)
    bg_color = button->bg;
  else
  {
    GdkRGBA *bc;
    gtk_style_context_get(context, state, "background-color", &bc, NULL);
    bg_color = *bc;
    gdk_rgba_free(bc);
  }
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
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width;
  int height = allocation.height;

  /* get the css geometry properties */
  GtkBorder margin, border, padding;
  gtk_style_context_get_margin(context, state, &margin);
  gtk_style_context_get_border(context, state, &border);
  gtk_style_context_get_padding(context, state, &padding);

  /* for button frame and background, remove css margin from allocation */
  int startx = margin.left;
  int starty = margin.top;
  int cwidth = width - margin.left - margin.right;
  int cheight = height - margin.top - margin.bottom;

  /* draw standard button background if not transparent nor flat styled */
  if((flags & CPF_STYLE_FLAT))
  {
    if(flags & CPF_PRELIGHT || (flags & CPF_ACTIVE && !(flags & CPF_BG_TRANSPARENT)))
    {
      // When CPF_BG_TRANSPARENT is set, change the background on
      // PRELIGHT, but not on ACTIVE
      if(!(flags & CPF_BG_TRANSPARENT) || (flags & CPF_PRELIGHT))
      {
        cairo_rectangle(cr, startx, starty, cwidth, cheight);
        gdk_cairo_set_source_rgba(cr, &bg_color);
        cairo_fill(cr);
      }
    }
    else if(!(flags & CPF_ACTIVE) || (flags & CPF_IGNORE_FG_STATE))
    {
      fg_color.alpha = CLAMP(fg_color.alpha / 2.0, 0.3, 1.0);
    }
  }
  else if(!(flags & CPF_BG_TRANSPARENT))
  {
    /* draw default boxed button */
    gtk_render_background(context, cr, startx, starty, cwidth, cheight);
    gtk_render_frame(context, cr, startx, starty, cwidth, cheight);
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
    /* calculate icon allocation */
    startx += border.left + padding.left;
    starty += border.top + padding.top;
    cwidth -= border.left + border.right + padding.left + padding.right;
    cheight -= border.top + border.bottom + padding.top + padding.bottom;

    /* we have to leave some breathing room to the cairo icon paint function to actually   */
    /* draw slightly outside the bounding box, for optical alignment and balancing of icons*/
    int overbook_x = round (0.125 * cwidth);
    int overbook_y = round (0.125 * cheight);
    startx += overbook_x;
    starty += overbook_y;
    cwidth -= 2 * overbook_x;
    cheight -= 2 * overbook_y;

    void *icon_data = DTGTK_TOGGLEBUTTON(widget)->icon_data;

    if(cwidth > 0 && cheight > 0)
        DTGTK_TOGGLEBUTTON(widget)->icon(cr, startx, starty, cwidth, cheight, flags, icon_data);
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
GtkWidget *dtgtk_togglebutton_new(DTGTKCairoPaintIconFunc paint, gint paintflags, void *paintdata)
{
  GtkDarktableToggleButton *button;
  button = g_object_new(dtgtk_togglebutton_get_type(), NULL);
  button->icon = paint;
  button->icon_flags = paintflags;
  button->icon_data = paintdata;
  gtk_widget_set_name(GTK_WIDGET(button), "dt-toggle-button");
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
  button->icon = paint;
  button->icon_flags = paintflags;
  button->icon_data = paintdata;
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
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
