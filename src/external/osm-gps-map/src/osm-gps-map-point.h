/* osm-gps-map-point.h */

#ifndef _OSM_GPS_MAP_POINT_H
#define _OSM_GPS_MAP_POINT_H

#include <glib.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define OSM_TYPE_GPS_MAP_POINT              osm_gps_map_point_get_type()

typedef struct _OsmGpsMapPoint OsmGpsMapPoint;

struct _OsmGpsMapPoint
{
    /* radians */
    float  rlat;
    float  rlon;
};

GType osm_gps_map_point_get_type (void) G_GNUC_CONST;

OsmGpsMapPoint *    osm_gps_map_point_new_degrees   (float lat, float lon);
OsmGpsMapPoint *    osm_gps_map_point_new_radians   (float rlat, float rlon);
void                osm_gps_map_point_get_degrees   (OsmGpsMapPoint *point, float *lat, float *lon);
void                osm_gps_map_point_get_radians   (OsmGpsMapPoint *point, float *rlat, float *rlon);
void                osm_gps_map_point_set_degrees   (OsmGpsMapPoint *point, float lat, float lon);
void                osm_gps_map_point_set_radians   (OsmGpsMapPoint *point, float rlat, float rlon);
void                osm_gps_map_point_free          (OsmGpsMapPoint *point);
OsmGpsMapPoint *    osm_gps_map_point_copy          (const OsmGpsMapPoint *point);

G_END_DECLS

#endif /* _OSM_GPS_MAP_POINT_H */
