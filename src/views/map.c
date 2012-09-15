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
#include "control/conf.h"
#include "common/debug.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "views/view.h"
#include "libs/lib.h"
#include "gui/drag_and_drop.h"

#include "osm-gps-map.h"

DT_MODULE(1)


typedef struct dt_map_t
{
  GtkWidget *center;
  OsmGpsMap *map;
  struct
  {
    sqlite3_stmt *main_query;
  } statements;
}
dt_map_t;

// needed for drag&drop
static GtkTargetEntry target_list[] = { { "image-id", GTK_TARGET_SAME_APP, DND_TARGET_IMGID } };
static guint n_targets = G_N_ELEMENTS (target_list);

/* proxy function to center map view on location at a zoom level */
static void _view_map_center_on_location(const dt_view_t *view, gdouble lon, gdouble lat, gdouble zoom);
/* callback when collection has change, needs to update map */
static void _view_map_collection_changed(gpointer instance, gpointer user_data);
/* callback when an image is selected in filmstrip, centers map */
static void _view_map_filmstrip_activate_callback(gpointer instance, gpointer user_data);
/* callback when an image is dropped from filmstrip */
static void drag_and_drop_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y, GtkSelectionData *selection_data,
                                   guint target_type, guint time, gpointer data);

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
  const int ts = 64;
  OsmGpsMapPoint bb[2], *l=NULL, *center=NULL;
  int px,py;
  dt_map_t *lib = (dt_map_t *)user_data;

  /* get bounding box coords */
  osm_gps_map_get_bbox(lib->map, &bb[0], &bb[1]);
  float bb_0_lat = 0.0, bb_0_lon = 0.0, bb_1_lat = 0.0, bb_1_lon = 0.0;
  osm_gps_map_point_get_degrees(&bb[0], &bb_0_lat, &bb_0_lon);
  osm_gps_map_point_get_degrees(&bb[1], &bb_1_lat, &bb_1_lon);

  /* make the bounding box a little bigger to the west and south */
  float lat0 = 0.0, lon0 = 0.0, lat1 = 0.0, lon1 = 0.0;
  OsmGpsMapPoint *pt0 = osm_gps_map_point_new_degrees(0.0, 0.0), *pt1 = osm_gps_map_point_new_degrees(0.0, 0.0);
  osm_gps_map_convert_screen_to_geographic(lib->map, 0, 0, pt0);
  osm_gps_map_convert_screen_to_geographic(lib->map, 1.5*ts, 1.5*ts, pt1);
  osm_gps_map_point_get_degrees(pt0, &lat0, &lon0);
  osm_gps_map_point_get_degrees(pt1, &lat1, &lon1);
  osm_gps_map_point_free(pt0);
  osm_gps_map_point_free(pt1);
  double south_border = lat0 - lat1, west_border = lon1 - lon0;

  /* get map view state and store  */
  int zoom = osm_gps_map_get_zoom(lib->map);
  center = osm_gps_map_get_center(lib->map);
  dt_conf_set_float("plugins/map/longitude", center->rlon);
  dt_conf_set_float("plugins/map/latitude", center->rlat);
  dt_conf_set_int("plugins/map/zoom", zoom);
  osm_gps_map_point_free(center);

  /* let's reset and reuse the main_query statement */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->statements.main_query);
  DT_DEBUG_SQLITE3_RESET(lib->statements.main_query);

  /* bind bounding box coords for the main query */
  DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->statements.main_query, 1, bb_0_lon - west_border);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->statements.main_query, 2, bb_1_lon);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->statements.main_query, 3, bb_0_lat);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->statements.main_query, 4, bb_1_lat - south_border);

  /* query collection ids */
  while(sqlite3_step(lib->statements.main_query) == SQLITE_ROW)
  {
    int32_t imgid = sqlite3_column_int(lib->statements.main_query, 0);

    cairo_set_source_rgba(cri, 0, 0, 0, 0.4);

    /* free l if allocated */
    if (l)
      osm_gps_map_point_free(l);

    /* for each image check if within bbox */
    const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, imgid);
    double longitude = cimg->longitude;
    double latitude  = cimg->latitude;
    dt_image_cache_read_release(darktable.image_cache, cimg);
    if(isnan(latitude) || isnan(longitude))
      continue;
    l = osm_gps_map_point_new_degrees(latitude, longitude);

    /* translate l into screen coords */
    osm_gps_map_convert_geographic_to_screen(lib->map, l, &px, &py);

    /* dependent on scale draw different overlays */
    if (zoom >= 14)
    {
      dt_mipmap_buffer_t buf;
      dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, ts, ts);
      dt_mipmap_cache_read_get(darktable.mipmap_cache, &buf, imgid, mip, 0);

      cairo_surface_t *surface = NULL;
      if(buf.buf)
      {
        float ms = fminf(
                     ts/(float)buf.width,
                     ts/(float)buf.height);

#if 0
        // this doesn't work since osm-gps-map always gives 0/0 as mouse coords :(
        /* find out if the cursor is over the image */
        if(pointerx >= px && pointerx <= (px + buf.width*ms + 4) && pointery <= (py - 8) && pointery >= (py - buf.height*ms - 8 - 4))
        {
          printf("over\n");
          cairo_set_source_rgba(cri, 1, 0, 0, 0.7);
        }
//         else
//           printf("%d/%d, %d/%d\n", px, py, pointerx, pointery);
#endif
        const int32_t stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, buf.width);
        surface = cairo_image_surface_create_for_data (buf.buf, CAIRO_FORMAT_RGB24,
                  buf.width, buf.height, stride);

        cairo_pattern_set_filter(cairo_get_source(cri), CAIRO_FILTER_NEAREST);
        cairo_save(cri);

        /* first of lets draw a pin */
        cairo_move_to(cri, px, py);
        cairo_line_to(cri, px+8, py-8);
        cairo_line_to(cri, px+4, py-8);
        cairo_fill(cri);

        /* and the frame around image */
        cairo_move_to(cri, px+2, py-8);
        cairo_line_to(cri, px+2 + (buf.width*ms) + 4, py-8);
        cairo_line_to(cri, px+2 + (buf.width*ms) + 4 , py-8-(buf.height*ms) - 4);
        cairo_line_to(cri, px+2 , py-8-(buf.height*ms) - 4);
        cairo_fill(cri);


        /* draw image*/
        cairo_translate(cri, px+4, py - 8 - (buf.height*ms) - 2);
        cairo_scale(cri, ms, ms);
        cairo_set_source_surface (cri, surface, 0, 0);
        cairo_paint(cri);

        cairo_restore(cri);

        cairo_surface_destroy(surface);

      }
    }
    else
    {
      /* just draw a patch indicating that there is images at the location */
      cairo_rectangle(cri, px-8, py-8, 16, 16);
      cairo_fill(cri);
    }
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

  /* restore last zoom,location in map */
  OsmGpsMapPoint *pt;
  float lon, lat;
  const float rlon = dt_conf_get_float("plugins/map/longitude");
  const float rlat = dt_conf_get_float("plugins/map/latitude");
  const int zoom = dt_conf_get_int("plugins/map/zoom");

  pt = osm_gps_map_point_new_radians(rlat,rlon);
  osm_gps_map_point_get_degrees (pt, &lat, &lon);
  osm_gps_map_set_center_and_zoom(lib->map, lat, lon, zoom);
  osm_gps_map_point_free(pt);

  _view_map_collection_changed(NULL, self);

  /* connect signal for filmstrip image activate */
  dt_control_signal_connect(darktable.signals,
                            DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE,
                            G_CALLBACK(_view_map_filmstrip_activate_callback),
                            self);

  /* allow drag&drop of images from filmstrip */
  gtk_drag_dest_set(GTK_WIDGET(lib->map), GTK_DEST_DEFAULT_ALL, target_list, n_targets, GDK_ACTION_COPY);
  g_signal_connect(GTK_WIDGET(lib->map), "drag-data-received", G_CALLBACK(drag_and_drop_received), self);

}

void leave(dt_view_t *self)
{
  /* disconnect from filmstrip image activate */
  dt_control_signal_disconnect(darktable.signals,
                               G_CALLBACK(_view_map_filmstrip_activate_callback),
                               (gpointer)self);

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

  /* build the new query string */
  char *geo_query = g_strdup("select id from images where \
                              longitude >= ?1 and longitude <= ?2 and latitude <= ?3 and latitude >= ?4\
                              and longitude not NULL and latitude not NULL limit 0, 100");

  /* prepare a new main query statement for collection */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), geo_query, -1, &lib->statements.main_query, NULL);

  dt_control_queue_redraw_widget(GTK_WIDGET(lib->map));

  g_free(geo_query);
}

static void _view_map_filmstrip_activate_callback(gpointer instance, gpointer user_data)
{
  double longitude, latitude;
  int32_t imgid = 0;
  if ((imgid=dt_view_filmstrip_get_activated_imgid(darktable.view_manager))>0)
  {
    const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, imgid);
    longitude = cimg->longitude;
    latitude = cimg->latitude;
    dt_image_cache_read_release(darktable.image_cache, cimg);
    if(!isnan(longitude) && !isnan(latitude))
      _view_map_center_on_location((dt_view_t*)user_data, longitude, latitude, 16); // TODO: is it better to keep the zoom level?
  }
}

static void
drag_and_drop_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y, GtkSelectionData *selection_data,
                       guint target_type, guint time, gpointer data)
{
  dt_view_t *self = (dt_view_t*)data;
  dt_map_t *lib = (dt_map_t*)self->data;

  gboolean success = FALSE;

  if((selection_data != NULL) && (selection_data->length >= 0) && target_type == DND_TARGET_IMGID)
  {
    float longitude, latitude;
    int *imgid = (int*)selection_data->data;
    if(imgid > 0)
    {
      OsmGpsMapPoint *pt = osm_gps_map_point_new_degrees(0.0, 0.0);
      osm_gps_map_convert_screen_to_geographic(lib->map, x, y, pt);
      osm_gps_map_point_get_degrees(pt, &latitude, &longitude);
      osm_gps_map_point_free(pt);

      const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, *imgid);
      dt_image_t *img = dt_image_cache_write_get(darktable.image_cache, cimg);
      img->longitude = longitude;
      img->latitude = latitude;
      dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_SAFE);
      dt_image_cache_read_release(darktable.image_cache, cimg);

      success = TRUE;
    }
  }
  gtk_drag_finish(context, success, FALSE, time);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;