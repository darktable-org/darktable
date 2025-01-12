/*
    This file is part of darktable,
    Copyright (C) 2025 darktable developers.

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

#include "titlebar.h"
#include <dwmapi.h>
#include <gdk/gdkwin32.h>

// This is taken from: https://gitlab.gnome.org/GNOME/gimp/-/blob/master/app/widgets/gimpwidgets-utils.c#L2655
// Set win32 title bar color based on theme (background color)
// Note: This function explicitly realizes the widget 
#ifdef _WIN32
void dtwin_set_titlebar_color(GtkWidget *widget)
{
  HWND hwnd;
  GdkWindow *window = NULL;
  GtkStyleContext *style;
  GdkRGBA *color = NULL;
  gboolean use_dark_mode = FALSE;

  gtk_widget_realize(widget); // creates GdkWindow but does not show the dialog
  window = gtk_widget_get_window(GTK_WIDGET(widget));
  if(window)
  {
    // if the background color is below the threshold, then we're
    // likely in dark mode
    style = gtk_widget_get_style_context(widget);
    gtk_style_context_get(style, gtk_style_context_get_state(style),
                          GTK_STYLE_PROPERTY_BACKGROUND_COLOR, &color,
                          NULL);
    if(color)
    {
      if(color->red * color->alpha < 0.5
          && color->green * color->alpha < 0.5
          && color->blue * color->alpha < 0.5)
      {
        use_dark_mode = TRUE;
      }

      gdk_rgba_free(color);
    }

    hwnd = (HWND) gdk_win32_window_get_handle(window);
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                          &use_dark_mode, sizeof(use_dark_mode));
  }
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
