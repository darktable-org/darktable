/* osd-utils.h
 *
 * Copyright (C) 2010 John Stowers <john.stowers@gmail.com>
 *
 * This is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __OSD_UTILS_H__
#define __OSD_UTILS_H__

#include <cairo.h>
#include <gdk/gdk.h>

typedef enum {
    OSD_NONE,
    OSD_UP,
    OSD_DOWN,
    OSD_LEFT,
    OSD_RIGHT,
    OSD_GPS,
    OSD_OUT,
    OSD_IN
} OsdControlPress_t;

char *osd_latitude_str(float latitude);
char *osd_longitude_str(float longitude);
int osd_render_centered_text(cairo_t *cr, int y, int width, int font_size, char *text);
void osd_render_crosshair_shape(cairo_t *cr, int w, int h, int r, int tick);
void osd_shape_shadow(cairo_t *cr);
void osd_shape(cairo_t *cr, GdkColor *bg, GdkColor *fg);
void osd_render_zoom(cairo_t *cr, gint x, gint y, gint w, gint h, gint gps, gint shadow, GdkColor *bg, GdkColor *fg);
void osd_render_dpad(cairo_t *cr, gint x, gint y, gint r, gint gps, gint shadow, GdkColor *bg, GdkColor *fg);
void osd_draw_bbox(cairo_t *cr, gint x, gint y, gint w, gint h);
gboolean osm_gps_map_in_circle(gint x, gint y, gint cx, gint cy, gint rad);
OsdControlPress_t osd_check_dpad(gint x, gint y, gint r, gboolean has_gps);
OsdControlPress_t osd_check_zoom(gint x, gint y, guint w, guint h, guint gps_w);
void osd_render_gps(cairo_t *cr, gint x, gint y, gint r, GdkColor *bg, GdkColor *fg);


#endif /* __OSD_UTILS_H__ */

