/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4; tab-width: 4 -*- */
/* vim:set et sw=4 ts=4 */
/*
 * Copyright (C) 2013 John Stowers <john.stowers@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <glib.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <cairo.h>

#include "osd-utils.h"

#define DEBUG_DRAWING   0

/* these can be overwritten with versions that support
   localization */
#define OSD_COORDINATES_CHR_N  "N"
#define OSD_COORDINATES_CHR_S  "S"
#define OSD_COORDINATES_CHR_E  "E"
#define OSD_COORDINATES_CHR_W  "W"

static void
debug_bbox(cairo_t *cr, gint x, gint y, gint w, gint h) {
#if DEBUG_DRAWING
    osd_draw_bbox(cr, x, y, w, h);
#endif
}

/* this is the classic geocaching notation */
char *
osd_latitude_str(float latitude) {
    char *c = OSD_COORDINATES_CHR_N;
    float integral, fractional;
    
    if(isnan(latitude)) 
        return NULL;
    
    if(latitude < 0) { 
        latitude = fabs(latitude); 
        c = OSD_COORDINATES_CHR_S; 
    }

    fractional = modff(latitude, &integral);
    
    return g_strdup_printf("%s %02d° %06.3f'", 
                           c, (int)integral, fractional*60.0);
}

char *
osd_longitude_str(float longitude) {
    char *c = OSD_COORDINATES_CHR_E;
    float integral, fractional;
    
    if(isnan(longitude)) 
        return NULL;
    
    if(longitude < 0) { 
        longitude = fabs(longitude); 
        c = OSD_COORDINATES_CHR_W; 
    }

    fractional = modff(longitude, &integral);
    
    return g_strdup_printf("%s %03d° %06.3f'", 
                           c, (int)integral, fractional*60.0);
}

/* render a string at the given screen position */
int
osd_render_centered_text(cairo_t *cr, int y, int width, int font_size, char *text) {
    if(!text) return y;

    char *p = g_malloc(strlen(text)+4);  // space for "...\n"
    strcpy(p, text);

    cairo_text_extents_t extents;
    memset(&extents, 0, sizeof(cairo_text_extents_t));
    cairo_text_extents (cr, p, &extents);
    g_assert(extents.width != 0.0);

    /* check if text needs to be truncated */
    int trunc_at = strlen(text);
    while(extents.width > width) {

        /* cut off all utf8 multibyte remains so the actual */
        /* truncation only deals with one byte */
        while((p[trunc_at-1] & 0xc0) == 0x80) {
            trunc_at--;
            g_assert(trunc_at > 0);
        }

        trunc_at--;
        g_assert(trunc_at > 0);

        strcpy(p+trunc_at, "...");
        cairo_text_extents (cr, p, &extents);
    }

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_set_line_width (cr, font_size/6);
    cairo_move_to (cr, (width - extents.width)/2, y - extents.y_bearing);
    cairo_text_path (cr, p);
    cairo_stroke (cr);

    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_move_to (cr, (width - extents.width)/2, y - extents.y_bearing);
    cairo_show_text (cr, p);

    g_free(p);

    /* skip + 1/5 line */
    return y + 6*font_size/5;
}

void
osd_render_crosshair_shape(cairo_t *cr, int w, int h, int r, int tick) {
    cairo_arc (cr, w/2, h/2,
               r, 0,  2*M_PI);

    cairo_move_to (cr, w/2 - r,
                   h/2);
    cairo_rel_line_to (cr, -tick, 0);
    cairo_move_to (cr, w/2 + r,
                   h/2);
    cairo_rel_line_to (cr,  tick, 0);

    cairo_move_to (cr, w/2,
                   h/2 - r);
    cairo_rel_line_to (cr, 0, -tick);
    cairo_move_to (cr, w/2,
                   h/2 + r);
    cairo_rel_line_to (cr, 0, tick);

    cairo_stroke (cr);
}

void
osd_shape_shadow(cairo_t *cr) {
    cairo_set_source_rgba (cr, 0, 0, 0, 0.2);
    cairo_fill (cr);
    cairo_stroke (cr);
}

void
osd_shape(cairo_t *cr, GdkRGBA *bg, GdkRGBA *fg) {
    gdk_cairo_set_source_rgba(cr, bg);
    cairo_fill_preserve (cr);
    gdk_cairo_set_source_rgba(cr, fg);
    cairo_set_line_width (cr, 1);
    cairo_stroke (cr);
}

void
osd_draw_bbox(cairo_t *cr, gint x, gint y, gint w, gint h)
{
    cairo_move_to (cr, x,    y);
    cairo_rel_line_to (cr, w,  0);
    cairo_rel_line_to (cr, 0,  h);
    cairo_rel_line_to (cr, -w,  0);
    cairo_rel_line_to (cr, 0,  -h);

    cairo_set_source_rgba (cr, 1, 0, 0, 0.5);
//    cairo_fill (cr);
    cairo_stroke (cr);
}

/* create the cairo shape used for the zoom buttons */
static void
osd_zoom_shape(cairo_t *cr, gint x, gint y, gint w, gint h) {
    gint r = h/2;   /* radius of curved ends of zoom pad */

    x += r;    

    cairo_move_to     (cr, x,           y);
    cairo_rel_line_to (cr, w-2*r,       0);
    cairo_arc         (cr, x+w-2*r,     y+r, r, -M_PI/2,  M_PI/2);
    cairo_rel_line_to (cr, -(w-2*r),    0);
    cairo_arc         (cr, x,           y+r, r,  M_PI/2, -M_PI/2);

}

static void
osd_zoom_labels(cairo_t *cr, gint x, gint y, gint w, gint h) {
    gint r = h/2;   /* radius of curved ends of zoom pad */
    gint l = h/3;   /* length of lines that draw -/+ */

    x += r;

    cairo_move_to     (cr, x-l,         y+r);
    cairo_rel_line_to (cr, 2*l,         0);

    cairo_move_to     (cr, x+w-2*r,     y+r-l);
    cairo_rel_line_to (cr, 0,           2*l);
    cairo_move_to     (cr, x+w-l-2*r,   y+r);
    cairo_rel_line_to (cr, 2*l,         0);
}

void
osd_render_zoom(cairo_t *cr, gint x, gint y, gint w, gint h, gint gps, gint shadow, GdkRGBA *bg, GdkRGBA *fg) {
    /* add the width of the GPS widget */
    w += gps;

    if (shadow) {
        osd_zoom_shape(cr, x+shadow, y+shadow, w, h);
        osd_shape_shadow(cr);
    }
    osd_zoom_shape(cr, x, y, w, h);
    osd_shape(cr, bg, fg);
    osd_zoom_labels(cr, x, y, w, h);
    osd_shape(cr, bg, fg);

    debug_bbox(cr, x, y, w, h);
}

static void 
osd_dpad_shape(cairo_t *cr, gint x, gint y, gint r) {
    cairo_arc (cr, x+r, y+r, r, 0, 2 * M_PI);
}

static void
osd_dpad_labels(cairo_t *cr, gint x, gint y, gint r) {
    /* move reference to dpad center */
    x += r;
    y += r;

    double D_TIP = 4*r/5;   /* distance of arrow tip from dpad center */
    double D_LEN = r/4;     /* length of arrow */
    double D_WID = D_LEN;   /* width of arrow */

    /* left arrow/triangle */
    cairo_move_to (cr, x + (-D_TIP+D_LEN), y + (-D_WID));
    cairo_rel_line_to (cr, -D_LEN, D_WID);
    cairo_rel_line_to (cr, +D_LEN, D_WID);

    /* right arrow/triangle */
    cairo_move_to (cr, x + (+D_TIP-D_LEN), y + (-D_WID));
    cairo_rel_line_to (cr, +D_LEN, D_WID);
    cairo_rel_line_to (cr, -D_LEN, D_WID);

    /* top arrow/triangle */
    cairo_move_to (cr, x + (-D_WID), y + (-D_TIP+D_LEN));
    cairo_rel_line_to (cr, D_WID, -D_LEN);
    cairo_rel_line_to (cr, D_WID, +D_LEN);

    /* bottom arrow/triangle */
    cairo_move_to (cr, x + (-D_WID), y + (+D_TIP-D_LEN));
    cairo_rel_line_to (cr, D_WID, +D_LEN);
    cairo_rel_line_to (cr, D_WID, -D_LEN);
}

void
osd_render_dpad(cairo_t *cr, gint x, gint y, gint r, gint gps, gint shadow, GdkRGBA *bg, GdkRGBA *fg) {
    if (shadow) {
        osd_dpad_shape(cr, x+shadow, y+shadow, r);
        osd_shape_shadow(cr);
    }
    osd_dpad_shape(cr, x, y, r);
    osd_shape(cr, bg, fg);
    osd_dpad_labels(cr, x, y, r);
    osd_shape(cr, bg, fg);

    debug_bbox(cr, x, y, 2*r, 2*r);
}

gboolean
osm_gps_map_in_circle(gint x, gint y, gint cx, gint cy, gint rad) 
{
    return( pow(cx - x, 2) + pow(cy - y, 2) < rad * rad);
}

OsdControlPress_t
osd_check_dpad(gint x, gint y, gint r, gboolean has_gps) 
{
    /* within entire dpad circle */
    if( osm_gps_map_in_circle(x, y, r, r, r)) {
        /* convert into position relative to dpads centre */
        x -= r;
        y -= r;

        if (has_gps)
            if( osm_gps_map_in_circle(x, y, 0, 0, r/3)) 
                return OSD_GPS;

        if( y < 0 && abs(x) < abs(y))
            return OSD_UP;

        if( y > 0 && abs(x) < abs(y))
            return OSD_DOWN;

        if( x < 0 && abs(y) < abs(x))
            return OSD_LEFT;

        if( x > 0 && abs(y) < abs(x))
            return OSD_RIGHT;
    }
    return OSD_NONE;
}

OsdControlPress_t
osd_check_zoom(gint x, gint y, guint w, guint h, guint gps_w) {

//osd_zoom_shape(cairo_t *cr, gint x, gint y, gint w, gint h) {
//gint r = h/2;   /* radius of curved ends of zoom pad */
//
//x += r;    
//
//cairo_move_to     (cr, x,           y);
//cairo_rel_line_to (cr, w-2*r,       0);
//cairo_arc         (cr, x+w-2*r,     y+r, r, -M_PI/2,  M_PI/2);
//cairo_rel_line_to (cr, -(w-2*r),    0);
//cairo_arc         (cr, x,           y+r, r,  M_PI/2, -M_PI/2);


    /* within entire zoom area */
    if( x > 0 && x < w && y > 0 && y < h) {
        gint r = h/2;   /* radius of curved ends of zoom pad */

        /* within circle around (-) label */
        if( osm_gps_map_in_circle(x, y, r, r, r))
            return OSD_OUT;

        /* within circle around (+) label */
        if( osm_gps_map_in_circle(x, y, w-r, r, r)) 
            return OSD_IN;

//#if Z_GPS == 1
//        /* within square around center */
//        if( x > Z_CENTER - Z_RAD && x < Z_CENTER + Z_RAD)
//            return OSD_GPS;
//#endif
//
//        /* between center of (-) button and center of entire zoom control area */
//        if(x > OSD_LEFT && x < D_RAD) 
//            return OSD_OUT;
//
//        /* between center of (+) button and center of entire zoom control area */
//        if(x < OSD_RIGHT && x > D_RAD) 
//            return OSD_IN;
    }
 
    return OSD_NONE;
}


/* draw a satellite receiver dish */
void
osd_render_gps(cairo_t *cr, gint x, gint y, gint w, GdkRGBA *bg, GdkRGBA *fg) {

    gint ox = x;
    gint oy = y;

    /* these define the gps widget shape */
    double GPS_V0 = 1.5*w/7.0;
    double GPS_V1 = 1.5*w/10.0;
    double GPS_V2 = 1.5*w/5.0;

    /* move reference to bounding box center */
    x += (2*w/5);
    y += (2*w/3);

    cairo_move_to (cr, x-GPS_V0, y+GPS_V0);
    cairo_rel_line_to (cr, +GPS_V0, -GPS_V0);
    cairo_rel_line_to (cr, +GPS_V0, +GPS_V0);
    cairo_close_path (cr);

    cairo_move_to (cr, x+GPS_V1-GPS_V2, y-2*GPS_V2);
    cairo_curve_to (cr, x-GPS_V2, y, x+GPS_V1, y+GPS_V1, x+GPS_V1+GPS_V2, y);
    cairo_close_path (cr);

    x += GPS_V1;
    cairo_move_to (cr, x, y-GPS_V2);
    cairo_rel_line_to (cr, +GPS_V1, -GPS_V1);

    osd_shape(cr, bg, fg);

    debug_bbox(cr, ox, oy, w, w);
}

