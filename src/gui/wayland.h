/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#pragma once

#include <gtk/gtk.h>

// initialize on the GTK thread before starting image processing
// the transport encoding remains fixed for the lifetime of the display
void dt_wayland_color_init(GdkDisplay *display);
// disable only during startup, before image processing
void dt_wayland_color_disable(void);
gboolean dt_wayland_color_available(void);
void dt_wayland_color_prepare_window(GtkWidget *window);

// call draw helpers on the GTK thread; bracket each widget draw with begin/end
void dt_wayland_color_begin(GtkWidget *widget, cairo_t *cr);
void dt_wayland_color_end(cairo_t *cr);
// reuse the last image while loading
gboolean dt_wayland_color_repaint(cairo_t *cr);

// paint the current source, encoded as Rec2020 gamma22, beneath the GTK overlay
// FALSE leaves cr unchanged; the caller must draw a BT709 gamma22 fallback
gboolean dt_wayland_color_paint(cairo_t *cr);
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
