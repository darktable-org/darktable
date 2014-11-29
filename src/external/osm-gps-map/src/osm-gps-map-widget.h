/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4; tab-width: 4 -*- */
/* vim:set et sw=4 ts=4 */
/*
 * Copyright (C) 2013 John Stowers <john.stowers@gmail.com>
 * Copyright (C) Marcus Bauer 2008 <marcus.bauer@gmail.com>
 * Copyright (C) Till Harbaum 2009 <till@harbaum.org>
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

#ifndef _OSM_GPS_MAP_WIDGET_H_
#define _OSM_GPS_MAP_WIDGET_H_

#include <glib.h>
#include <glib-object.h>
#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cairo.h>

G_BEGIN_DECLS

#define OSM_TYPE_GPS_MAP             (osm_gps_map_get_type ())
#define OSM_GPS_MAP(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), OSM_TYPE_GPS_MAP, OsmGpsMap))
#define OSM_GPS_MAP_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), OSM_TYPE_GPS_MAP, OsmGpsMapClass))
#define OSM_IS_GPS_MAP(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OSM_TYPE_GPS_MAP))
#define OSM_IS_GPS_MAP_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), OSM_TYPE_GPS_MAP))
#define OSM_GPS_MAP_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), OSM_TYPE_GPS_MAP, OsmGpsMapClass))

typedef struct _OsmGpsMapClass OsmGpsMapClass;
typedef struct _OsmGpsMap OsmGpsMap;
typedef struct _OsmGpsMapPrivate OsmGpsMapPrivate;

#include "osm-gps-map-layer.h"
#include "osm-gps-map-point.h"
#include "osm-gps-map-track.h"
#include "osm-gps-map-image.h"

struct _OsmGpsMapClass
{
    GtkDrawingAreaClass parent_class;

    void (*draw_gps_point) (OsmGpsMap *map, cairo_t *cr);
};

struct _OsmGpsMap
{
    GtkDrawingArea parent_instance;
    OsmGpsMapPrivate *priv;
};

typedef enum {
    OSM_GPS_MAP_KEY_FULLSCREEN,
    OSM_GPS_MAP_KEY_ZOOMIN,
    OSM_GPS_MAP_KEY_ZOOMOUT,
    OSM_GPS_MAP_KEY_UP,
    OSM_GPS_MAP_KEY_DOWN,
    OSM_GPS_MAP_KEY_LEFT,
    OSM_GPS_MAP_KEY_RIGHT,
    OSM_GPS_MAP_KEY_MAX
} OsmGpsMapKey_t;

#define OSM_GPS_MAP_INVALID         (0.0/0.0)
#define OSM_GPS_MAP_CACHE_DISABLED  "none://"
#define OSM_GPS_MAP_CACHE_AUTO      "auto://"
#define OSM_GPS_MAP_CACHE_FRIENDLY  "friendly://"

GType           osm_gps_map_get_type                    (void) G_GNUC_CONST;

GtkWidget*      osm_gps_map_new                         (void);

gchar*          osm_gps_map_get_default_cache_directory (void);

void            osm_gps_map_download_maps               (OsmGpsMap *map, OsmGpsMapPoint *pt1, OsmGpsMapPoint *pt2, int zoom_start, int zoom_end);
void            osm_gps_map_download_cancel_all         (OsmGpsMap *map);
void            osm_gps_map_get_bbox                    (OsmGpsMap *map, OsmGpsMapPoint *pt1, OsmGpsMapPoint *pt2);
void            osm_gps_map_set_center_and_zoom         (OsmGpsMap *map, float latitude, float longitude, int zoom);
void            osm_gps_map_set_center                  (OsmGpsMap *map, float latitude, float longitude);
int             osm_gps_map_set_zoom                    (OsmGpsMap *map, int zoom);
void            osm_gps_map_set_zoom_offset             (OsmGpsMap *map, int zoom_offset);
int             osm_gps_map_zoom_in                     (OsmGpsMap *map);
int             osm_gps_map_zoom_out                    (OsmGpsMap *map);
void            osm_gps_map_scroll                      (OsmGpsMap *map, gint dx, gint dy);
float           osm_gps_map_get_scale                   (OsmGpsMap *map);
void            osm_gps_map_set_keyboard_shortcut       (OsmGpsMap *map, OsmGpsMapKey_t key, guint keyval);
void            osm_gps_map_track_add                   (OsmGpsMap *map, OsmGpsMapTrack *track);
void            osm_gps_map_track_remove_all            (OsmGpsMap *map);
gboolean        osm_gps_map_track_remove                (OsmGpsMap *map, OsmGpsMapTrack *track);
void            osm_gps_map_gps_add                     (OsmGpsMap *map, float latitude, float longitude, float heading);
void            osm_gps_map_gps_clear                   (OsmGpsMap *map);
OsmGpsMapTrack *osm_gps_map_gps_get_track               (OsmGpsMap *map);
OsmGpsMapImage *osm_gps_map_image_add                   (OsmGpsMap *map, float latitude, float longitude, GdkPixbuf *image);
OsmGpsMapImage *osm_gps_map_image_add_z                 (OsmGpsMap *map, float latitude, float longitude, GdkPixbuf *image, gint zorder);
OsmGpsMapImage *osm_gps_map_image_add_with_alignment    (OsmGpsMap *map, float latitude, float longitude, GdkPixbuf *image, float xalign, float yalign);
OsmGpsMapImage *osm_gps_map_image_add_with_alignment_z  (OsmGpsMap *map, float latitude, float longitude, GdkPixbuf *image, float xalign, float yalign, gint zorder);
gboolean        osm_gps_map_image_remove                (OsmGpsMap *map, OsmGpsMapImage *image);
void            osm_gps_map_image_remove_all            (OsmGpsMap *map);
void            osm_gps_map_layer_add                   (OsmGpsMap *map, OsmGpsMapLayer *layer);
gboolean        osm_gps_map_layer_remove                (OsmGpsMap *map, OsmGpsMapLayer *layer);
void            osm_gps_map_layer_remove_all            (OsmGpsMap *map);
void            osm_gps_map_convert_screen_to_geographic(OsmGpsMap *map, gint pixel_x, gint pixel_y, OsmGpsMapPoint *pt);
void            osm_gps_map_convert_geographic_to_screen(OsmGpsMap *map, OsmGpsMapPoint *pt, gint *pixel_x, gint *pixel_y);
OsmGpsMapPoint *osm_gps_map_get_event_location          (OsmGpsMap *map, GdkEventButton *event);

G_END_DECLS

#endif /* _OSM_GPS_MAP_WIDGET_H_ */

