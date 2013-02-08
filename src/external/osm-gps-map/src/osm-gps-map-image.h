/* osm-gps-map-image.h */

#ifndef _OSM_GPS_MAP_IMAGE_H
#define _OSM_GPS_MAP_IMAGE_H

#include <glib-object.h>
#include <gdk/gdk.h>

#include "osm-gps-map-point.h"

G_BEGIN_DECLS

#define OSM_TYPE_GPS_MAP_IMAGE              osm_gps_map_image_get_type()
#define OSM_GPS_MAP_IMAGE(obj)              (G_TYPE_CHECK_INSTANCE_CAST ((obj), OSM_TYPE_GPS_MAP_IMAGE, OsmGpsMapImage))
#define OSM_GPS_MAP_IMAGE_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), OSM_TYPE_GPS_MAP_IMAGE, OsmGpsMapImageClass))
#define OSM_IS_GPS_MAP_IMAGE(obj)           (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OSM_TYPE_GPS_MAP_IMAGE))
#define OSM_IS_GPS_MAP_IMAGE_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), OSM_TYPE_GPS_MAP_IMAGE))
#define OSM_GPS_MAP_IMAGE_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), OSM_TYPE_GPS_MAP_IMAGE, OsmGpsMapImageClass))

typedef struct _OsmGpsMapImage OsmGpsMapImage;
typedef struct _OsmGpsMapImageClass OsmGpsMapImageClass;
typedef struct _OsmGpsMapImagePrivate OsmGpsMapImagePrivate;

struct _OsmGpsMapImage
{
    GObject parent;

    OsmGpsMapImagePrivate *priv;
};

struct _OsmGpsMapImageClass
{
    GObjectClass parent_class;
};

GType osm_gps_map_image_get_type (void) G_GNUC_CONST;

OsmGpsMapImage *osm_gps_map_image_new (void);
void            osm_gps_map_image_draw (OsmGpsMapImage *object, GdkDrawable *drawable, GdkGC *gc, GdkRectangle *rect);
const OsmGpsMapPoint *osm_gps_map_image_get_point(OsmGpsMapImage *object);
gint osm_gps_map_image_get_zorder(OsmGpsMapImage *object);

G_END_DECLS

#endif /* _OSM_GPS_MAP_IMAGE_H */
