/* osm-gps-map-osd-classic.h
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

#ifndef __OSM_GPS_MAP_OSD_H__
#define __OSM_GPS_MAP_OSD_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define OSM_TYPE_GPS_MAP_OSD            (osm_gps_map_osd_get_type())
#define OSM_GPS_MAP_OSD(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj),  OSM_TYPE_GPS_MAP_OSD, OsmGpsMapOsd))
#define OSM_GPS_MAP_OSD_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),   OSM_TYPE_GPS_MAP_OSD, OsmGpsMapOsdClass))
#define OSM_IS_GPS_MAP_OSD(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj),  OSM_TYPE_GPS_MAP_OSD))
#define OSM_IS_GPS_MAP_OSD_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),   OSM_TYPE_GPS_MAP_OSD))
#define OSM_GPS_MAP_OSD_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),   OSM_TYPE_GPS_MAP_OSD, OsmGpsMapOsdClass))

typedef struct _OsmGpsMapOsd        OsmGpsMapOsd;
typedef struct _OsmGpsMapOsdClass   OsmGpsMapOsdClass;
typedef struct _OsmGpsMapOsdPrivate OsmGpsMapOsdPrivate;

struct _OsmGpsMapOsd
{
    GObject parent;

	/*< private >*/
	OsmGpsMapOsdPrivate *priv;
};

struct _OsmGpsMapOsdClass
{
	GObjectClass parent_class;

	/* vtable */
	
};

GType               osm_gps_map_osd_get_type (void);
OsmGpsMapOsd*       osm_gps_map_osd_new      (void);

G_END_DECLS

#endif /* __OSM_GPS_MAP_OSD_H__ */
