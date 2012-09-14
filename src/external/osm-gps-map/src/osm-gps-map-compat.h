#ifndef _OSM_GPS_MAP_COMPAT_H
#define _OSM_GPS_MAP_COMPAT_H

#include "osm-gps-map-widget.h"

G_BEGIN_DECLS

/* Depreciated Functions */
#define coord_t OsmGpsMapPoint
void        osm_gps_map_add_track                   (OsmGpsMap *map, GSList *track)                                     G_GNUC_DEPRECATED;
void        osm_gps_map_replace_track               (OsmGpsMap *map, GSList *old_track, GSList *new_track)              G_GNUC_DEPRECATED;
void        osm_gps_map_clear_tracks                (OsmGpsMap *map)                                                    G_GNUC_DEPRECATED;
void        osm_gps_map_draw_gps                    (OsmGpsMap *map, float latitude, float longitude, float heading)    G_GNUC_DEPRECATED;
void        osm_gps_map_clear_gps                   (OsmGpsMap *map)                                                    G_GNUC_DEPRECATED;
void        osm_gps_map_add_image                   (OsmGpsMap *map, float latitude, float longitude, GdkPixbuf *image) G_GNUC_DEPRECATED;
void        osm_gps_map_add_image_with_alignment    (OsmGpsMap *map, float latitude, float longitude, GdkPixbuf *image, float xalign, float yalign) G_GNUC_DEPRECATED;
gboolean    osm_gps_map_remove_image                (OsmGpsMap *map, GdkPixbuf *image)                                  G_GNUC_DEPRECATED;
void        osm_gps_map_clear_images                (OsmGpsMap *map)                                                    G_GNUC_DEPRECATED;
void        osm_gps_map_add_layer                   (OsmGpsMap *map, OsmGpsMapLayer *layer)                             G_GNUC_DEPRECATED;
void        osm_gps_map_screen_to_geographic        (OsmGpsMap *map, gint pixel_x, gint pixel_y, gfloat *latitude, gfloat *longitude) G_GNUC_DEPRECATED;
void        osm_gps_map_geographic_to_screen        (OsmGpsMap *map, gfloat latitude, gfloat longitude, gint *pixel_x, gint *pixel_y) G_GNUC_DEPRECATED;
OsmGpsMapPoint  osm_gps_map_get_co_ordinates        (OsmGpsMap *map, int pixel_x, int pixel_y)                          G_GNUC_DEPRECATED;
void        osm_gps_map_set_mapcenter               (OsmGpsMap *map, float latitude, float longitude, int zoom)         G_GNUC_DEPRECATED;

G_END_DECLS

#endif /* _OSM_GPS_MAP_COMPAT_H */
