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

/**
 * SECTION:osm-gps-map-layer
 * @short_description: An interface for layers to be drawn on the map
 * @stability: Stable
 * @include: osm-gps-map.h
 *
 * #OsmGpsMapLayer is an interface implemented by objects that wish
 * to draw on top of the map respond to button press events. The most
 * common implementation of this interface is #OsmGpsMapOsd
 **/

#include "osm-gps-map-layer.h"

GType osm_gps_map_layer_get_type()
{
	static GType object_type = 0;
	if (!object_type) {
		static const GTypeInfo object_info = {
			sizeof(OsmGpsMapLayerIface),
			NULL,	/* base init */
			NULL,	/* base finalize */
		};
		object_type =
		    g_type_register_static(G_TYPE_INTERFACE,
					   "OsmGpsMapLayer",
					   &object_info, 0);
	}
	return object_type;
}

void
osm_gps_map_layer_render (OsmGpsMapLayer *self, OsmGpsMap *map)
{
	OSM_GPS_MAP_LAYER_GET_INTERFACE (self)->render (self, map);
}

void
osm_gps_map_layer_draw (OsmGpsMapLayer *self, OsmGpsMap *map, cairo_t *cr)
{
	OSM_GPS_MAP_LAYER_GET_INTERFACE (self)->draw (self, map, cr);
}

gboolean
osm_gps_map_layer_busy (OsmGpsMapLayer *self)
{
	return OSM_GPS_MAP_LAYER_GET_INTERFACE (self)->busy (self);
}

gboolean
osm_gps_map_layer_button_press (OsmGpsMapLayer *self, OsmGpsMap *map, GdkEventButton *event)
{
	return OSM_GPS_MAP_LAYER_GET_INTERFACE (self)->button_press (self, map, event);
}

