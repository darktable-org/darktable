#ifndef _OSM_GPS_MAP_SOURCE_H_
#define _OSM_GPS_MAP_SOURCE_H_

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    OSM_GPS_MAP_SOURCE_NULL,
    OSM_GPS_MAP_SOURCE_OPENSTREETMAP,
    OSM_GPS_MAP_SOURCE_OPENSTREETMAP_RENDERER,
    OSM_GPS_MAP_SOURCE_OPENAERIALMAP,
    OSM_GPS_MAP_SOURCE_MAPS_FOR_FREE,
    OSM_GPS_MAP_SOURCE_OPENCYCLEMAP,
    OSM_GPS_MAP_SOURCE_OSM_PUBLIC_TRANSPORT,
    OSM_GPS_MAP_SOURCE_GOOGLE_STREET,
    OSM_GPS_MAP_SOURCE_GOOGLE_SATELLITE,
    OSM_GPS_MAP_SOURCE_GOOGLE_HYBRID,
    OSM_GPS_MAP_SOURCE_VIRTUAL_EARTH_STREET,
    OSM_GPS_MAP_SOURCE_VIRTUAL_EARTH_SATELLITE,
    OSM_GPS_MAP_SOURCE_VIRTUAL_EARTH_HYBRID,
    OSM_GPS_MAP_SOURCE_YAHOO_STREET,
    OSM_GPS_MAP_SOURCE_YAHOO_SATELLITE,
    OSM_GPS_MAP_SOURCE_YAHOO_HYBRID,
    OSM_GPS_MAP_SOURCE_OSMC_TRAILS,

    OSM_GPS_MAP_SOURCE_LAST
} OsmGpsMapSource_t;

const char* osm_gps_map_source_get_friendly_name    (OsmGpsMapSource_t source);
const char* osm_gps_map_source_get_repo_uri         (OsmGpsMapSource_t source);
const char* osm_gps_map_source_get_image_format     (OsmGpsMapSource_t source);
int         osm_gps_map_source_get_min_zoom         (OsmGpsMapSource_t source);
int         osm_gps_map_source_get_max_zoom         (OsmGpsMapSource_t source);
gboolean    osm_gps_map_source_is_valid             (OsmGpsMapSource_t source);

G_END_DECLS

#endif /* _OSM_GPS_MAP_SOURCE_H_ */
