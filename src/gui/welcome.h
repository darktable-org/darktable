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

// Opaque handle.
typedef struct _dt_welcome_screen_t dt_welcome_screen_t;

// Allocate an empty welcome screen.
dt_welcome_screen_t *dt_welcome_screen_new(void);

// Append an empty page.  Returns the page index (0-based) to use with the
// dt_welcome_screen_page_add_*() functions.
int dt_welcome_screen_add_page(dt_welcome_screen_t *ws);

// Add a conf-key-driven widget to a page.
// The widget type (checkbox / combobox / directory chooser) and its label
// are inferred from the darktableconfig.xml metadata for conf_key.
// description is optional (NULL = none); it accepts Pango markup
// (e.g. <b>, <i>, <a href="...">).
void dt_welcome_screen_page_add_conf(dt_welcome_screen_t *ws,
                                     int                  page_idx,
                                     const char          *conf_key,
                                     const char          *description);

// Add a plain paragraph of text to a page (Pango markup supported).
// Pass centered=TRUE to centre-align the text horizontally.
void dt_welcome_screen_page_add_paragraph(dt_welcome_screen_t *ws,
                                          int                  page_idx,
                                          const char          *text,
                                          gboolean             centered);

// Add a directory-chooser row to a page.
// Reads and writes a string conf key holding the directory path.
// Use this for string keys that hold path patterns (e.g. containing
// $(PICTURES_FOLDER)) which cannot be typed as 'dir' in the XML.
// description is optional (NULL = none).
void dt_welcome_screen_page_add_dirchooser(dt_welcome_screen_t *ws,
                                           int                  page_idx,
                                           const char          *label,
                                           const char          *description,
                                           const char          *conf_key);

// Build the GTK dialog and block until the user closes it.
void dt_welcome_screen_show(dt_welcome_screen_t *ws);

// Free all resources (call after dt_welcome_screen_show returns).
void dt_welcome_screen_free(dt_welcome_screen_t *ws);

// High-level entry point: checks the "ui/show_welcome_screen" preference,
// builds and shows the screen on first run, then clears the flag.
void dt_welcome_screen_run_if_needed(void);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
