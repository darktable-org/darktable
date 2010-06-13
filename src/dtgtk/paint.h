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
#ifndef DTGTK_PAINT_H
#define DTGTK_PAINT_H

#include <gtk/gtk.h>
#include <cairo.h>

typedef enum dtgtk_cairo_paint_flags_t {
	CPF_DIRECTION_UP=1,
	CPF_DIRECTION_DOWN=2,
	CPF_DIRECTION_LEFT=4,
	CPF_DIRECTION_RIGHT=8,
	CPF_ACTIVE=16,
	CPF_PRELIGHT=32,
	CPF_IGNORE_FG_STATE=64	// Ignore state when setting foregroundcolor 
	
} dtgtk_cairo_paint_flags_t;


typedef void (*DTGTKCairoPaintIconFunc)(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags);

/** Paint a arrow left or right */
void dtgtk_cairo_paint_arrow(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags);
/** Paint a store icon */
void dtgtk_cairo_paint_store(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags);
/** Paint a reset icon */
void dtgtk_cairo_paint_reset(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags);
/** Paint a reset icon */
void dtgtk_cairo_paint_presets(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags);
/** Paint a flip icon */
void dtgtk_cairo_paint_flip(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags);
/** Paint a switch icon */
void dtgtk_cairo_paint_switch(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags);
/** Paint a color rect icon */
void dtgtk_cairo_paint_color(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags);
/** Paint a eye icon */
void dtgtk_cairo_paint_eye(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags);
/** Paint a timer icon */
void dtgtk_cairo_paint_timer(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags);
/** Paint a filmstrip icon */
void dtgtk_cairo_paint_filmstrip(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags);
/** Paint a directory icon */
void dtgtk_cairo_paint_directory(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags);


#endif
