
/*
		This file is part of darktable,
		copyright (c) 2011 henrik andersson.

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

#include "control/control.h"
#include "common/debug.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/image_cache.h"
#include "views/view.h"
#include "libs/lib.h"

#include "osm-gps-map.h"

DT_MODULE(1)


typedef struct dt_map_t
{
  GtkWidget *center;
  OsmGpsMap *map;
  struct {
    sqlite3_stmt *main_query;
  }statements;
}
dt_map_t;

/* proxy function to center map view on location at a zoom level */
static void _view_map_center_on_location(const dt_view_t *view, gdouble lon, gdouble lat, gdouble zoom);
/* callback when collection has change, needs to update map */
static void _view_map_collection_changed(gpointer instance, gpointer user_data);

const char *name(dt_view_t *self)
{
  return _("map");
}

uint32_t view(dt_view_t *self)
{
  return DT_VIEW_MAP;
}

void init(dt_view_t *self)
{
  self->data = malloc(sizeof(dt_map_t));
  memset(self->data,0,sizeof(dt_map_t));


  dt_map_t *lib = (dt_map_t *)self->data;

  lib->map = g_object_new (OSM_TYPE_GPS_MAP,
		      "map-source", OSM_GPS_MAP_SOURCE_OPENSTREETMAP,
		      "tile-cache", "dt.map.cache",
		      "tile-cache-base", "/tmp",
		      "proxy-uri",g_getenv("http_proxy"),
		      NULL);

  OsmGpsMapLayer *osd = g_object_new (OSM_TYPE_GPS_MAP_OSD,
				 "show-scale",TRUE, NULL);

  osm_gps_map_layer_add(OSM_GPS_MAP(lib->map), osd);
  g_object_unref(G_OBJECT(osd));

}

void cleanup(dt_view_t *self)
{
  free(self->data);
}


void configure(dt_view_t *self, int wd, int ht)
{
  //dt_capture_t *lib=(dt_capture_t*)self->data;
}

static void _view_map_post_expose(cairo_t *cri, int32_t width_i, int32_t height_i, 
				  int32_t pointerx, int32_t pointery, gpointer user_data)
{
  OsmGpsMapPoint bb[2], *l;
  int px,py;
  dt_map_t *lib = (dt_map_t *)user_data;

  osm_gps_map_get_bbox(lib->map, &bb[0], &bb[1]);

  cairo_set_source_rgba(cri, 0, 0, 0, 0.4);

  /* let's reset and reuse the main_query statement */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->statements.main_query);
  DT_DEBUG_SQLITE3_RESET(lib->statements.main_query);

  /* setup offset and row for the main query */
  DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 1, 0);
  DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 2, 100);

  /* query collection ids */
  while(sqlite3_step(lib->statements.main_query) == SQLITE_ROW)
  {
    /* for each image check if within bbox */
    const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, 
						     sqlite3_column_int(lib->statements.main_query, 0));

    l = osm_gps_map_point_new_degrees(cimg->latitude, cimg->longitude);

    osm_gps_map_convert_geographic_to_screen(lib->map, l, &px, &py);

    osm_gps_map_point_free(l);

    cairo_rectangle(cri, px-8, py-8, 16, 16);
    cairo_fill(cri);
  }

}

int try_enter(dt_view_t *self)
{
  return 0;
}


void enter(dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;

  lib->map = OSM_GPS_MAP(osm_gps_map_new());

  /* replace center widget */
  GtkWidget *parent = gtk_widget_get_parent(dt_ui_center(darktable.gui->ui));
  gtk_widget_hide(dt_ui_center(darktable.gui->ui));  
  gtk_box_pack_start(GTK_BOX(parent), GTK_WIDGET(lib->map) ,TRUE, TRUE, 0);

  gtk_box_reorder_child(GTK_BOX(parent), GTK_WIDGET(lib->map), 2);

  gtk_widget_show_all(GTK_WIDGET(lib->map));

  /* setup proxy functions */
  darktable.view_manager->proxy.map.view = self;
  darktable.view_manager->proxy.map.center_on_location = _view_map_center_on_location;

  /* setup collection listener and initialize main_query statement */
  dt_control_signal_connect(darktable.signals, 
			    DT_SIGNAL_COLLECTION_CHANGED, 
			    G_CALLBACK(_view_map_collection_changed),
			    (gpointer) self);


  osm_gps_map_set_post_expose_callback(lib->map, _view_map_post_expose, lib);
  

  _view_map_collection_changed(NULL, self);

}

void leave(dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  gtk_widget_destroy(GTK_WIDGET(lib->map));
  gtk_widget_show_all(dt_ui_center(darktable.gui->ui));

  /* reset proxy */
  darktable.view_manager->proxy.map.view = NULL;

  /* disconnect from signals */
  dt_control_signal_disconnect(darktable.signals, 
			       G_CALLBACK(_view_map_collection_changed), (gpointer)self);

}

void mouse_moved(dt_view_t *self, double x, double y, int which)
{
  // redraw center on mousemove
  dt_control_queue_redraw_center();
}

void init_key_accels(dt_view_t *self)
{
#if 0
  // Setup key accelerators in capture view...
  dt_accel_register_view(self, NC_("accel", "toggle film strip"),
                         GDK_f, GDK_CONTROL_MASK);
#endif
}

void connect_key_accels(dt_view_t *self)
{
#if 0
  GClosure *closure = g_cclosure_new(G_CALLBACK(film_strip_key_accel),
                                     (gpointer)self, NULL);
  dt_accel_connect_view(self, "toggle film strip", closure);
#endif
}


void _view_map_center_on_location(const dt_view_t *view, gdouble lon, gdouble lat, gdouble zoom)
{
  dt_map_t *lib = (dt_map_t *)view->data;
  osm_gps_map_set_center_and_zoom(lib->map, lat, lon, zoom);
}


void _view_map_collection_changed(gpointer instance, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_map_t *lib = (dt_map_t *)self->data;
  
  /* check if we can get a query from collection */
  const gchar *query = dt_collection_get_query (darktable.collection);
  if(!query)
    return;

  /* if we have a statment lets clean it */
  if(lib->statements.main_query)
    sqlite3_finalize(lib->statements.main_query);

  /* prepare a new main query statement for collection */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &lib->statements.main_query, NULL);

  dt_control_queue_redraw_widget(GTK_WIDGET(lib->map));

}
