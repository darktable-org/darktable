/*
    This file is part of darktable,
    Copyright (C) 2011-2023 darktable developers.

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

#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/gpx.h"
#include "common/geo.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/drag_and_drop.h"
#include "gui/draw.h"
#include "libs/lib.h"
#include "views/view.h"
#include "views/view_api.h"
#include <gdk/gdkkeysyms.h>

#include <osm-gps-map.h>

DT_MODULE(1)

typedef struct dt_geo_position_t
{
  double x, y;
  int cluster_id;
  dt_imgid_t imgid;
} dt_geo_position_t;

typedef struct dt_map_image_t
{
  dt_imgid_t imgid;
  double latitude;
  double longitude;
  int group;
  int group_count;
  gboolean group_same_loc;
  gboolean selected_in_group;
  OsmGpsMapImage *image;
  gint width, height;
  int thumbnail;
} dt_map_image_t;

typedef struct dt_map_t
{
  gboolean entering;
  OsmGpsMap *map;
  OsmGpsMapSource_t map_source;
  OsmGpsMapLayer *osd;
  GSList *images;
  dt_geo_position_t *points;
  int nb_points;
  GdkPixbuf *image_pin, *place_pin;
  GList *selected_images;
  gboolean start_drag;
  int start_drag_x, start_drag_y;
  int start_drag_offset_x, start_drag_offset_y;
  float thumb_lat_angle, thumb_lon_angle;
  sqlite3_stmt *main_query;
  gboolean drop_filmstrip_activated;
  gboolean filter_images_drawn;
  int max_images_drawn;
  dt_map_box_t bbox;
  int time_out;
  int timeout_event_source;
  int thumbnail;
  dt_map_image_t *last_hovered_entry;
  struct
  {
    dt_location_draw_t main;
    gboolean drag;
    int time_out;
    GList *others;
  } loc;
} dt_map_t;

#define UNCLASSIFIED -1
#define NOISE -2

#define CORE_POINT 1
#define NOT_CORE_POINT 0

static const int thumb_size = 128, thumb_border = 2, image_pin_size = 13, place_pin_size = 72;
static const int cross_size = 16, max_size = 1024;
static const uint32_t thumb_frame_color = 0x000000aa;
static const uint32_t thumb_frame_sel_color = 0xffffffee;
static const uint32_t thumb_frame_gpx_color = 0xff000099;
static const uint32_t pin_outer_color = 0x0000aaaa;
static const uint32_t pin_inner_color = 0xffffffee;
static const uint32_t pin_line_color = 0x000000ff;

typedef enum dt_map_thumb_t
{
  DT_MAP_THUMB_THUMB = 0,
  DT_MAP_THUMB_COUNT,
  DT_MAP_THUMB_NONE
} dt_map_thumb_t;

/* proxy function to center map view on location at a zoom level */
static void _view_map_center_on_location(const dt_view_t *view, gdouble lon, gdouble lat, gdouble zoom);
/* proxy function to center map view on a bounding box */
static void _view_map_center_on_bbox(const dt_view_t *view, gdouble lon1, gdouble lat1, gdouble lon2, gdouble lat2);
/* proxy function to show or hide the osd */
static void _view_map_show_osd(const dt_view_t *view);
/* proxy function to set the map source */
static void _view_map_set_map_source(const dt_view_t *view, OsmGpsMapSource_t map_source);
/* wrapper for setting the map source in the GObject */
static void _view_map_set_map_source_g_object(const dt_view_t *view, OsmGpsMapSource_t map_source);
/* proxy function to check if preferences have changed */
static void _view_map_check_preference_changed(gpointer instance, gpointer user_data);
/* proxy function to add a marker to the map */
static GObject *_view_map_add_marker(const dt_view_t *view, dt_geo_map_display_t type, GList *points);
/* proxy function to remove a marker from the map */
static gboolean _view_map_remove_marker(const dt_view_t *view, dt_geo_map_display_t type, GObject *marker);
/* proxy function to add a location to the map */
static void _view_map_add_location(const dt_view_t *view, dt_map_location_data_t *g, const guint locid);
/* proxy function to remove a location from the map */
static void _view_map_location_action(const dt_view_t *view, const int action);
/* proxy function to provide a drag context icon */
static void _view_map_drag_set_icon(const dt_view_t *self, GdkDragContext *context,
                             const dt_imgid_t imgid, const int count);

/* callback when the collection changes */
static void _view_map_collection_changed(gpointer instance, dt_collection_change_t query_change,
                                         dt_collection_properties_t changed_property, gpointer imgs, int next,
                                         gpointer user_data);
/* callback when the selection changes */
static void _view_map_selection_changed(gpointer instance, gpointer user_data);
/* callback when images geotags change */
static void _view_map_geotag_changed(gpointer instance, GList *imgs, const int locid, gpointer user_data);
/* update the geotag information on location tag */
static void _view_map_update_location_geotag(dt_view_t *self);
/* callback when an image is selected in filmstrip, centers map */
static void _view_map_filmstrip_activate_callback(gpointer instance, dt_imgid_t imgid, gpointer user_data);
/* callback when an image is dropped from filmstrip */
static void _drag_and_drop_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                    GtkSelectionData *selection_data, guint target_type, guint time,
                                    gpointer data);
/* callback when the user drags images FROM the map */
static void _view_map_dnd_get_callback(GtkWidget *widget, GdkDragContext *context,
                                       GtkSelectionData *selection_data, guint target_type, guint time,
                                       dt_view_t *self);
/* callback that readds the images to the map */
static void _view_map_changed_callback(OsmGpsMap *map, dt_view_t *self);
/* callback that handles mouse scroll */
static gboolean _view_map_scroll_event(GtkWidget *w, GdkEventScroll *event, dt_view_t *self);
/* callback that handles clicks on the map */
static gboolean _view_map_button_press_callback(GtkWidget *w, GdkEventButton *e, dt_view_t *self);
static gboolean _view_map_button_release_callback(GtkWidget *w, GdkEventButton *e, dt_view_t *self);
/* callback when the mouse is moved */
static gboolean _view_map_motion_notify_callback(GtkWidget *w, GdkEventMotion *e, dt_view_t *self);
static gboolean _view_map_drag_motion_callback(GtkWidget *widget, GdkDragContext *dc,
                                               gint x, gint y, guint time, dt_view_t *self);
static gboolean _view_map_dnd_failed_callback(GtkWidget *widget, GdkDragContext *drag_context,
                                              GtkDragResult result, dt_view_t *self);
static void _view_map_dnd_remove_callback(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                          GtkSelectionData *selection_data, guint target_type, guint time,
                                          gpointer data);
// find the images clusters on the map
static void _dbscan(dt_geo_position_t *points, unsigned int num_points, double epsilon,
                    unsigned int minpts);
static gboolean _view_map_prefs_changed(dt_map_t *lib);
static void _view_map_build_main_query(dt_map_t *lib);

/* center map to on the baricenter of the image list */
static gboolean _view_map_center_on_image_list(dt_view_t *self, const char *table);
/* center map on the given image */
static void _view_map_center_on_image(dt_view_t *self, const dt_imgid_t imgid);

static OsmGpsMapPolygon *_view_map_add_polygon_location(dt_map_t *lib, dt_location_draw_t *ld);
static gboolean _view_map_remove_polygon(const dt_view_t *view, OsmGpsMapPolygon *polygon);
static OsmGpsMapImage *_view_map_draw_location(dt_map_t *lib, dt_location_draw_t *ld,
                                               const gboolean main);
static void _view_map_remove_location(dt_map_t *lib, dt_location_draw_t *ld);

const char *name(const dt_view_t *self)
{
  return _("map");
}

uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_MAP;
}

#ifdef USE_LUA

static int latitude_member(lua_State *L)
{
  dt_view_t *module = *(dt_view_t **)lua_touserdata(L, 1);
  dt_map_t *lib = (dt_map_t *)module->data;
  if(lua_gettop(L) != 3)
  {
    if(dt_view_manager_get_current_view(darktable.view_manager) != module)
    {
      lua_pushnumber(L, dt_conf_get_float("plugins/map/latitude"));
    }
    else
    {
      float value;
      g_object_get(G_OBJECT(lib->map), "latitude", &value, NULL);
      lua_pushnumber(L, value);
    }
    return 1;
  }
  else
  {
    luaL_checktype(L, 3, LUA_TNUMBER);
    float lat = lua_tonumber(L, 3);
    lat = CLAMP(lat, -90, 90);
    if(dt_view_manager_get_current_view(darktable.view_manager) != module)
    {
      dt_conf_set_float("plugins/map/latitude", lat);
    }
    else
    {
      float value;
      g_object_get(G_OBJECT(lib->map), "longitude", &value, NULL);
      osm_gps_map_set_center(lib->map, lat, value);
    }
    return 0;
  }
}

static int longitude_member(lua_State *L)
{
  dt_view_t *module = *(dt_view_t **)lua_touserdata(L, 1);
  dt_map_t *lib = (dt_map_t *)module->data;
  if(lua_gettop(L) != 3)
  {
    if(dt_view_manager_get_current_view(darktable.view_manager) != module)
    {
      lua_pushnumber(L, dt_conf_get_float("plugins/map/longitude"));
    }
    else
    {
      float value;
      g_object_get(G_OBJECT(lib->map), "longitude", &value, NULL);
      lua_pushnumber(L, value);
    }
    return 1;
  }
  else
  {
    luaL_checktype(L, 3, LUA_TNUMBER);
    float longi = lua_tonumber(L, 3);
    longi = CLAMP(longi, -180, 180);
    if(dt_view_manager_get_current_view(darktable.view_manager) != module)
    {
      dt_conf_set_float("plugins/map/longitude", longi);
    }
    else
    {
      float value;
      g_object_get(G_OBJECT(lib->map), "latitude", &value, NULL);
      osm_gps_map_set_center(lib->map, value, longi);
    }
    return 0;
  }
}

static int zoom_member(lua_State *L)
{
  dt_view_t *module = *(dt_view_t **)lua_touserdata(L, 1);
  dt_map_t *lib = (dt_map_t *)module->data;
  if(lua_gettop(L) != 3)
  {
    if(dt_view_manager_get_current_view(darktable.view_manager) != module)
    {
      lua_pushnumber(L, dt_conf_get_float("plugins/map/zoom"));
    }
    else
    {
      int value;
      g_object_get(G_OBJECT(lib->map), "zoom", &value, NULL);
      lua_pushnumber(L, value);
    }
    return 1;
  }
  else
  {
    // we rely on osm to correctly clamp zoom (checked in osm source
    // lua can have temporarily false values but it will fix itself when entering map
    // unfortunately we can't get the min max when lib->map doesn't exist
    luaL_checktype(L, 3, LUA_TNUMBER);
    const int zoom = luaL_checkinteger(L, 3);
    if(dt_view_manager_get_current_view(darktable.view_manager) != module)
    {
      dt_conf_set_int("plugins/map/zoom", zoom);
    }
    else
    {
      osm_gps_map_set_zoom(lib->map, zoom);
    }
    return 0;
  }
}
#endif // USE_LUA

#ifndef HAVE_OSMGPSMAP_110_OR_NEWER
// the following functions were taken from libosmgpsmap
// Copyright (C) Marcus Bauer 2008 <marcus.bauer@gmail.com>
// Copyright (C) 2013 John Stowers <john.stowers@gmail.com>
// Copyright (C) 2014 Martijn Goedhart <goedhart.martijn@gmail.com>

#if FLT_RADIX == 2
  #define LOG2(x) (ilogb(x))
#else
  #define LOG2(x) ((int)floor(log2(abs(x))))
#endif

#define TILESIZE 256

static float deg2rad(float deg)
{
  return (deg * M_PI / 180.0);
}

static int latlon2zoom(int pix_height, int pix_width, float lat1, float lat2, float lon1, float lon2)
{
  const float lat1_m = atanh(sinf(lat1));
  const float lat2_m = atanh(sinf(lat2));
  const int zoom_lon = LOG2((double)(2 * pix_width * M_PI) / (TILESIZE * (lon2 - lon1)));
  const int zoom_lat = LOG2((double)(2 * pix_height * M_PI) / (TILESIZE * (lat2_m - lat1_m)));
  return MIN(zoom_lon, zoom_lat);
}

#undef LOG2
#undef TILESIZE

//  Copyright (C) 2013 John Stowers <john.stowers@gmail.com>
//  Copyright (C) Marcus Bauer 2008 <marcus.bauer@gmail.com>
//  Copyright (C) John Stowers 2009 <john.stowers@gmail.com>
//  Copyright (C) Till Harbaum 2009 <till@harbaum.org>
//
//  Contributions by
//  Everaldo Canuto 2009 <everaldo.canuto@gmail.com>
static void osm_gps_map_zoom_fit_bbox(OsmGpsMap *map, float latitude1, float latitude2, float longitude1, float longitude2)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(GTK_WIDGET (map), &allocation);
  const int zoom = latlon2zoom(allocation.height, allocation.width,
                               deg2rad(latitude1), deg2rad(latitude2),
                               deg2rad(longitude1), deg2rad(longitude2));
  osm_gps_map_set_center(map, (latitude1 + latitude2) / 2, (longitude1 + longitude2) / 2);
  osm_gps_map_set_zoom(map, zoom);
}
#endif // HAVE_OSMGPSMAP_110_OR_NEWER

static void _toast_log_lat_lon(const float lat, const float lon)
{
  gchar *latitude = dt_util_latitude_str(lat);
  gchar *longitude = dt_util_longitude_str(lon);
  dt_toast_log("%s %s",latitude, longitude);
  g_free(latitude);
  g_free(longitude);
}

static GdkPixbuf *_view_map_images_count(const int nb_images, const gboolean same_loc,
                                         double *count_width, double *count_height)
{
  char text[8] = {0};
  snprintf(text, sizeof(text), "%d", nb_images > 99999 ? 99999 : nb_images);

  const int w = DT_PIXEL_APPLY_DPI(thumb_size + 2 * thumb_border);
  const int h = DT_PIXEL_APPLY_DPI(image_pin_size);

  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(cst);
  /* fill background */
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_MAP_COUNT_BG);
  cairo_paint(cr);

  dt_gui_gtk_set_source_rgb(cr, same_loc ? DT_GUI_COLOR_MAP_COUNT_SAME_LOC
                                         : DT_GUI_COLOR_MAP_COUNT_DIFF_LOC);
  cairo_set_font_size(cr, 12 * (1 + (darktable.gui->dpi_factor - 1) / 2));
  cairo_text_extents_t te;
  cairo_text_extents(cr, text, &te);
  *count_width = te.width + 4 * te.x_bearing;
  *count_height = te.height + 2;
  cairo_move_to(cr, te.x_bearing, te.height + 1);

  cairo_show_text(cr, text);
  cairo_destroy(cr);
  uint8_t *data = cairo_image_surface_get_data(cst);
  dt_draw_cairo_to_gdk_pixbuf(data, w, h);
  const size_t size = (size_t)w * h * 4;
  uint8_t *buf = (uint8_t *)malloc(size);
  memcpy(buf, data, size);
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(buf, GDK_COLORSPACE_RGB, TRUE, 8, w, h, w * 4,
                                               (GdkPixbufDestroyNotify)free, NULL);
  cairo_surface_destroy(cst);
  return pixbuf;
}

static GdkPixbuf *_init_image_pin()
{
  const size_t w = DT_PIXEL_APPLY_DPI(thumb_size + 2 * thumb_border);
  const size_t h = DT_PIXEL_APPLY_DPI(image_pin_size);
  g_return_val_if_fail(w > 0 && h > 0, NULL);

  float r, g, b, a;
  r = ((thumb_frame_color & 0xff000000) >> 24) / 255.0;
  g = ((thumb_frame_color & 0x00ff0000) >> 16) / 255.0;
  b = ((thumb_frame_color & 0x0000ff00) >> 8) / 255.0;
  a = ((thumb_frame_color & 0x000000ff) >> 0) / 255.0;

  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(cst);
  cairo_set_source_rgba(cr, r, g, b, a);
  dtgtk_cairo_paint_map_pin(cr, (h-w)/2, 0, w, h, 0, NULL); // keep the pin on left
  cairo_destroy(cr);
  uint8_t *data = cairo_image_surface_get_data(cst);
  dt_draw_cairo_to_gdk_pixbuf(data, w, h);
  const size_t size = (size_t)w * h * 4;
  uint8_t *buf = (uint8_t *)malloc(size);
  memcpy(buf, data, size);
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(buf, GDK_COLORSPACE_RGB, TRUE, 8, w, h, w * 4,
                                               (GdkPixbufDestroyNotify)free, NULL);
  cairo_surface_destroy(cst);
  return pixbuf;
}

static GdkPixbuf *_init_place_pin()
{
  const size_t w = DT_PIXEL_APPLY_DPI(place_pin_size);
  const size_t h = DT_PIXEL_APPLY_DPI(place_pin_size);
  g_return_val_if_fail(w > 0 && h > 0, NULL);

  float r, g, b, a;

  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(cst);

  // outer shape
  r = ((pin_outer_color & 0xff000000) >> 24) / 255.0;
  g = ((pin_outer_color & 0x00ff0000) >> 16) / 255.0;
  b = ((pin_outer_color & 0x0000ff00) >> 8) / 255.0;
  a = ((pin_outer_color & 0x000000ff) >> 0) / 255.0;
  cairo_set_source_rgba(cr, r, g, b, a);
  cairo_arc(cr, 0.5 * w, 0.333 * h, 0.333 * h - 2, 150.0 * (M_PI / 180.0), 30.0 * (M_PI / 180.0));
  cairo_line_to(cr, 0.5 * w, h - 2);
  cairo_close_path(cr);
  cairo_fill_preserve(cr);

  r = ((pin_line_color & 0xff000000) >> 24) / 255.0;
  g = ((pin_line_color & 0x00ff0000) >> 16) / 255.0;
  b = ((pin_line_color & 0x0000ff00) >> 8) / 255.0;
  a = ((pin_line_color & 0x000000ff) >> 0) / 255.0;
  cairo_set_source_rgba(cr, r, g, b, a);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1));
  cairo_stroke(cr);

  // inner circle
  r = ((pin_inner_color & 0xff000000) >> 24) / 255.0;
  g = ((pin_inner_color & 0x00ff0000) >> 16) / 255.0;
  b = ((pin_inner_color & 0x0000ff00) >> 8) / 255.0;
  a = ((pin_inner_color & 0x000000ff) >> 0) / 255.0;
  cairo_set_source_rgba(cr, r, g, b, a);
  cairo_arc(cr, 0.5 * w, 0.333 * h, 0.17 * h, 0, 2.0 * M_PI);
  cairo_fill(cr);

  cairo_destroy(cr);
  uint8_t *data = cairo_image_surface_get_data(cst);
  dt_draw_cairo_to_gdk_pixbuf(data, w, h);
  size_t size = (size_t)w * h * 4;
  uint8_t *buf = (uint8_t *)malloc(size);
  memcpy(buf, data, size);
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(buf, GDK_COLORSPACE_RGB, TRUE, 8, w, h, w * 4,
                                               (GdkPixbufDestroyNotify)free, NULL);
  cairo_surface_destroy(cst);
  return pixbuf;
}

static GdkPixbuf *_draw_ellipse(const float dlongitude, const float dlatitude,
                                const gboolean main)
{
  const int dlon = dlongitude > max_size ? max_size :
                   dlongitude < cross_size ? cross_size : dlongitude;
  const int dlat = dlatitude > max_size ? max_size :
                   dlatitude < cross_size ? cross_size : dlatitude;
  const gboolean landscape = dlon > dlat ? TRUE : FALSE;
  const float ratio = dlon > dlat ? (float)dlat / (float)dlon : (float)dlon / (float)dlat;
  const int w = DT_PIXEL_APPLY_DPI(2.0 * (landscape ? dlon : dlat));
  const int h = w;
  const int d = DT_PIXEL_APPLY_DPI(main ? 2 : 1);
  const int cross = DT_PIXEL_APPLY_DPI(cross_size);
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(cst);

  cairo_set_line_width(cr, d);
  const int color_hi = dlon == max_size || dlon == cross_size
                                ? main ? DT_GUI_COLOR_MAP_LOC_SHAPE_DEF
                                       : DT_GUI_COLOR_MAP_LOC_SHAPE_HIGH
                                : DT_GUI_COLOR_MAP_LOC_SHAPE_HIGH;

  cairo_matrix_t save_matrix;
  cairo_get_matrix(cr, &save_matrix);
  cairo_translate(cr, 0.5 * w, 0.5 * h);
  cairo_scale(cr, landscape ? 1 : ratio, landscape ? ratio : 1);
  cairo_translate(cr, -0.5 * w, -0.5 * h);

  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_MAP_LOC_SHAPE_LOW);
  cairo_arc(cr, 0.5 * w, 0.5 * h, 0.5 * h - d - d, 0, 2.0 * M_PI);

  cairo_set_matrix(cr, &save_matrix);
  cairo_stroke(cr);
  cairo_move_to(cr, 0.5 * w + d, 0.5 * h - cross);
  cairo_line_to(cr, 0.5 * w + d, 0.5 * h + cross);
  cairo_move_to(cr, 0.5 * w - cross, 0.5 * h - d);
  cairo_line_to(cr, 0.5 * w + cross, 0.5 * h - d);
  cairo_stroke(cr);

  cairo_get_matrix(cr, &save_matrix);
  cairo_translate(cr, 0.5 * w, 0.5 * h);
  cairo_scale(cr, landscape ? 1 : ratio, landscape ? ratio : 1);
  cairo_translate(cr, -0.5 * w, -0.5 * h);

  dt_gui_gtk_set_source_rgb(cr, color_hi);
  cairo_arc(cr, 0.5 * w, 0.5 * h, 0.5 * h - d, 0, 2.0 * M_PI);

  cairo_set_matrix(cr, &save_matrix);

  cairo_stroke(cr);
  cairo_move_to(cr, 0.5 * w, 0.5 * h - cross);
  cairo_line_to(cr, 0.5 * w, 0.5 * h + cross);
  cairo_move_to(cr, 0.5 * w - cross, 0.5 * h );
  cairo_line_to(cr, 0.5 * w + cross, 0.5 * h );
  cairo_stroke(cr);

  cairo_destroy(cr);
  uint8_t *data = cairo_image_surface_get_data(cst);
  dt_draw_cairo_to_gdk_pixbuf(data, w, h);
  size_t size = (size_t)w * h * 4;
  uint8_t *buf = (uint8_t *)malloc(size);
  memcpy(buf, data, size);
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(buf, GDK_COLORSPACE_RGB, TRUE, 8, w, h, w * 4,
                                               (GdkPixbufDestroyNotify)free, NULL);
  cairo_surface_destroy(cst);
  return pixbuf;
}

static GdkPixbuf *_draw_rectangle(const float dlongitude, const float dlatitude,
                                  const gboolean main)
{
  const int dlon = dlongitude > max_size ? max_size :
                  dlongitude < cross_size ? cross_size : dlongitude;
  const int dlat = dlatitude > max_size ? max_size :
                  dlatitude < cross_size ? cross_size : dlatitude;
  const int w = DT_PIXEL_APPLY_DPI(2.0 * dlon);
  const int h = DT_PIXEL_APPLY_DPI(2.0 * dlat);
  const int d = DT_PIXEL_APPLY_DPI(main ? 2 : 1);
  const int cross = DT_PIXEL_APPLY_DPI(cross_size);
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(cst);

  cairo_set_line_width(cr,d);
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_MAP_LOC_SHAPE_LOW);
  cairo_move_to(cr, d + d, d + d);
  cairo_line_to(cr, w - d - d, d + d);
  cairo_line_to(cr, w - d - d, h - d - d);
  cairo_line_to(cr, d + d, h - d - d);
  cairo_line_to(cr, d + d, d + d);
  cairo_move_to(cr, 0.5 * w + d, 0.5 * h - cross);
  cairo_line_to(cr, 0.5 * w + d, 0.5 * h + cross);
  cairo_move_to(cr, 0.5 * w - cross, 0.5 * h - d);
  cairo_line_to(cr, 0.5 * w + cross, 0.5 * h - d);
  cairo_stroke(cr);

  dt_gui_gtk_set_source_rgb(cr, dlon == max_size || dlon == cross_size ||
                                dlat == max_size || dlat == cross_size
                                ? main ? DT_GUI_COLOR_MAP_LOC_SHAPE_DEF
                                       : DT_GUI_COLOR_MAP_LOC_SHAPE_HIGH
                                : DT_GUI_COLOR_MAP_LOC_SHAPE_HIGH);
  cairo_move_to(cr, d, d);
  cairo_line_to(cr, w - d, d);
  cairo_line_to(cr, w - d, h - d);
  cairo_line_to(cr, d, h - d);
  cairo_line_to(cr, d, d);
  cairo_move_to(cr, 0.5 * w, 0.5 * h - cross);
  cairo_line_to(cr, 0.5 * w, 0.5 * h + cross);
  cairo_move_to(cr, 0.5 * w - cross, 0.5 * h );
  cairo_line_to(cr, 0.5 * w + cross, 0.5 * h );
  cairo_stroke(cr);

  cairo_destroy(cr);
  uint8_t *data = cairo_image_surface_get_data(cst);
  dt_draw_cairo_to_gdk_pixbuf(data, w, h);
  size_t size = (size_t)w * h * 4;
  uint8_t *buf = (uint8_t *)malloc(size);
  memcpy(buf, data, size);
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(buf, GDK_COLORSPACE_RGB, TRUE, 8, w, h, w * 4,
                                               (GdkPixbufDestroyNotify)free, NULL);
  cairo_surface_destroy(cst);
  return pixbuf;
}

void expose(dt_view_t *self, cairo_t *cri, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  if(lib->entering)
  {
    // we need to ensure there's no remaining things on canvas.
    // otherwise they can appear on map move
    lib->entering = FALSE;
    cairo_set_source_rgb(cri, 0, 0, 0);
    cairo_paint(cri);
  }
}

static void _view_changed(gpointer instance, dt_view_t *old_view,
                          dt_view_t *new_view, dt_view_t *self)
{
  if(old_view == self)
  {
    _view_map_location_action(self, MAP_LOCATION_ACTION_REMOVE);
  }
}

void init(dt_view_t *self)
{
  self->data = calloc(1, sizeof(dt_map_t));

  dt_map_t *lib = (dt_map_t *)self->data;

  darktable.view_manager->proxy.map.view = self;

  if(darktable.gui)
  {
    lib->image_pin = _init_image_pin();
    lib->place_pin = _init_place_pin();
    lib->drop_filmstrip_activated = FALSE;
    lib->thumb_lat_angle = 0.01, lib->thumb_lon_angle = 0.01;
    lib->time_out = 0, lib->timeout_event_source = 0;
    lib->loc.main.id = 0, lib->loc.main.location = NULL, lib->loc.time_out = 0;
    lib->loc.others = NULL;
    lib->last_hovered_entry = NULL;

    OsmGpsMapSource_t map_source
        = OSM_GPS_MAP_SOURCE_OPENSTREETMAP; // open street map should be a nice default ...
    const char *old_map_source = dt_conf_get_string_const("plugins/map/map_source");
    if(old_map_source && old_map_source[0] != '\0')
    {
      // find the number of the stored map_source
      for(int i = 0; i <= OSM_GPS_MAP_SOURCE_LAST; i++)
      {
        const gchar *new_map_source = osm_gps_map_source_get_friendly_name(i);
        if(!g_strcmp0(old_map_source, new_map_source))
        {
          if(osm_gps_map_source_is_valid(i)) map_source = i;
          break;
        }
      }
    }
    else
      dt_conf_set_string("plugins/map/map_source", osm_gps_map_source_get_friendly_name(map_source));

    lib->map_source = map_source;

    lib->map = g_object_new(OSM_TYPE_GPS_MAP, "map-source", OSM_GPS_MAP_SOURCE_NULL, "proxy-uri",
                            g_getenv("http_proxy"), NULL);

    g_object_ref(lib->map); // we want to keep map alive until explicit destroy

    lib->osd = g_object_new(OSM_TYPE_GPS_MAP_OSD, "show-scale", TRUE, "show-coordinates", TRUE, "show-dpad",
                            TRUE, "show-zoom", TRUE,
#ifdef HAVE_OSMGPSMAP_NEWER_THAN_110
                            "show-copyright", TRUE,
#endif
                            NULL);

    if(dt_conf_get_bool("plugins/map/show_map_osd"))
    {
      osm_gps_map_layer_add(OSM_GPS_MAP(lib->map), lib->osd);
    }

    gtk_drag_dest_set(GTK_WIDGET(lib->map), GTK_DEST_DEFAULT_ALL,
                      target_list_internal, n_targets_internal, GDK_ACTION_MOVE);
    g_signal_connect(GTK_WIDGET(lib->map), "scroll-event", G_CALLBACK(_view_map_scroll_event), self);
    g_signal_connect(GTK_WIDGET(lib->map), "drag-data-received", G_CALLBACK(_drag_and_drop_received), self);
    g_signal_connect(GTK_WIDGET(lib->map), "drag-data-get", G_CALLBACK(_view_map_dnd_get_callback), self);
    g_signal_connect(GTK_WIDGET(lib->map), "drag-failed", G_CALLBACK(_view_map_dnd_failed_callback), self);

    g_signal_connect(GTK_WIDGET(lib->map), "changed", G_CALLBACK(_view_map_changed_callback), self);
    g_signal_connect_after(G_OBJECT(lib->map), "button-press-event",
                           G_CALLBACK(_view_map_button_press_callback), self);
    g_signal_connect_after(G_OBJECT(lib->map), "button-release-event",
                          G_CALLBACK(_view_map_button_release_callback), self);
    g_signal_connect(G_OBJECT(lib->map), "motion-notify-event", G_CALLBACK(_view_map_motion_notify_callback),
                     self);
    g_signal_connect(G_OBJECT(lib->map), "drag-motion", G_CALLBACK(_view_map_drag_motion_callback),
                     self);
  }

  /* build the query string */
  lib->main_query = NULL;
  _view_map_build_main_query(lib);

#ifdef USE_LUA
  lua_State *L = darktable.lua_state.state;
  luaA_Type my_type = dt_lua_module_entry_get_type(L, "view", self->module_name);
  lua_pushcfunction(L, latitude_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register_type(L, my_type, "latitude");
  lua_pushcfunction(L, longitude_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register_type(L, my_type, "longitude");
  lua_pushcfunction(L, zoom_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register_type(L, my_type, "zoom");

#endif // USE_LUA
  /* connect collection changed signal */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_view_map_collection_changed), (gpointer)self);
  /* connect selection changed signal */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_view_map_selection_changed), (gpointer)self);
  /* connect preference changed signal */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE,
                            G_CALLBACK(_view_map_check_preference_changed), (gpointer)self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,
                            G_CALLBACK(_view_changed), (gpointer)self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_GEOTAG_CHANGED,
                            G_CALLBACK(_view_map_geotag_changed), (gpointer)self);
}

void cleanup(dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_view_map_collection_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_view_map_selection_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_view_map_check_preference_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_view_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_view_map_geotag_changed), self);

  if(darktable.gui)
  {
    g_object_unref(G_OBJECT(lib->image_pin));
    g_object_unref(G_OBJECT(lib->place_pin));
    g_object_unref(G_OBJECT(lib->osd));
    osm_gps_map_image_remove_all(lib->map);
    if(lib->points)
    {
      g_free(lib->points);
      lib->points = NULL;
    }
    if(lib->images)
    {
      g_slist_free_full(lib->images, g_free);
      lib->images = NULL;
    }
    if(lib->loc.main.id)
    {
      _view_map_remove_location(lib, &lib->loc.main);
      lib->loc.main.id = 0;
    }
    if(lib->loc.others)
    {
      for(GList *other = lib->loc.others; other; other = g_list_next(other))
      {
        dt_location_draw_t *d = (dt_location_draw_t *)other->data;
        _view_map_remove_location(lib, d);
        // polygons are freed only from others list
        dt_map_location_free_polygons(d);
      }
      g_list_free_full(lib->loc.others, g_free);
      lib->loc.others = NULL;
    }
    // FIXME: it would be nice to cleanly destroy the object, but we are doing this inside expose() so
    // removing the widget can cause segfaults.
    //     g_object_unref(G_OBJECT(lib->map));
  }
  if(lib->main_query) sqlite3_finalize(lib->main_query);
  free(self->data);
}

void configure(dt_view_t *self, int wd, int ht)
{
  // dt_capture_t *lib=(dt_capture_t*)self->data;
}

gboolean try_enter(dt_view_t *self)
{
  return FALSE;
}

static void _view_map_signal_change_raise(gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_view_map_geotag_changed), self);
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_view_map_collection_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_GEOTAG_CHANGED, (GList *)NULL, 0);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_view_map_collection_changed), self);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_view_map_geotag_changed), self);
}

// updating collection when mouse scrolls to resize the location is too demanding
// so wait for scrolling stop
static gboolean _view_map_signal_change_delayed(gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_map_t *lib = (dt_map_t *)self->data;
  if(lib->loc.time_out)
  {
    lib->loc.time_out--;
    if(!lib->loc.time_out)
    {
      _view_map_signal_change_raise(self);
      return FALSE;
    }
  }
  return TRUE;
}

static void _view_map_signal_change_wait(dt_view_t *self, const int time_out)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  if(time_out)
  {
    if(lib->loc.time_out)
    {
      lib->loc.time_out = time_out;
    }
    else
    {
      lib->loc.time_out = time_out;
      g_timeout_add(100, _view_map_signal_change_delayed, self);
    }
  }
  else _view_map_signal_change_raise(self);
}

static gboolean _view_map_redraw(gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_map_t *lib = (dt_map_t *)self->data;
  g_signal_emit_by_name(lib->map, "changed");
  return FALSE; // remove the function again
}

static void _view_map_angles(dt_map_t *lib, const gint pixels, const float lat,
                             const float lon, float *dlat_min, float *dlon_min)
{
  OsmGpsMapPoint *pt0 = osm_gps_map_point_new_degrees(lat, lon);
  OsmGpsMapPoint *pt1 = osm_gps_map_point_new_degrees(0.0, 0.0);
  gint pixel_x = 0, pixel_y = 0;
  osm_gps_map_convert_geographic_to_screen(lib->map, pt0, &pixel_x, &pixel_y);
  osm_gps_map_convert_screen_to_geographic(lib->map, pixel_x + pixels,
                                           pixel_y + pixels, pt1);
  float lat1 = 0.0, lon1 = 0.0;
  osm_gps_map_point_get_degrees(pt1, &lat1, &lon1);
  osm_gps_map_point_free(pt0);
  osm_gps_map_point_free(pt1);
  *dlat_min = lat - lat1;
  *dlon_min = lon1 - lon;
}

// when map is moving we often get incorrect (even negative) values.
// keep the last positive values here to limit wrong effects (still not perfect)
static void _view_map_thumb_angles(dt_map_t *lib, const float lat, const float lon,
                                   float *dlat_min, float *dlon_min)
{
  _view_map_angles(lib, thumb_size, lat, lon, dlat_min, dlon_min);
  if(*dlat_min > 0.0 && *dlon_min > 0.0)
  {
    lib->thumb_lat_angle = *dlat_min;
    lib->thumb_lon_angle = *dlon_min;
  }
  else // something got wrong, keep the last positive values
  {
    *dlat_min = lib->thumb_lat_angle ;
    *dlon_min = lib->thumb_lon_angle;
  }
}

static float _view_map_angles_to_pixels(const dt_map_t *lib, const float lat0,
                                        const float lon0, const float angle)
{
  OsmGpsMapPoint *pt0 = osm_gps_map_point_new_degrees(lat0, lon0);
  OsmGpsMapPoint *pt1 = osm_gps_map_point_new_degrees(lat0 + angle, lon0 + angle);
  gint px0 = 0, py0 = 0;
  gint px1 = 0, py1 = 0;
  osm_gps_map_convert_geographic_to_screen(lib->map, pt0, &px0, &py0);
  osm_gps_map_convert_geographic_to_screen(lib->map, pt1, &px1, &py1);
  osm_gps_map_point_free(pt0);
  osm_gps_map_point_free(pt1);
  return abs(px1 - px0);
}

static double _view_map_get_angles_ratio(const dt_map_t *lib, const float lat0,
                                         const float lon0)
{
  OsmGpsMapPoint *pt0 = osm_gps_map_point_new_degrees(lat0, lon0);
  OsmGpsMapPoint *pt1 = osm_gps_map_point_new_degrees(lat0 + 2.0, lon0 + 2.0);
  gint px0 = 0, py0 = 0;
  gint px1 = 0, py1 = 0;
  osm_gps_map_convert_geographic_to_screen(lib->map, pt0, &px0, &py0);
  osm_gps_map_convert_geographic_to_screen(lib->map, pt1, &px1, &py1);
  osm_gps_map_point_free(pt0);
  osm_gps_map_point_free(pt1);
  double ratio = 1.;
  if((px1 - px0) > 0)
    ratio = (float)abs(py1 - py0) / (float)(px1 - px0);
  return ratio;
}

static GdkPixbuf *_draw_location(dt_map_t *lib, int *width, int *height,
                                 dt_map_location_data_t *data,
                                 const gboolean main)
{
  float pixel_lon = _view_map_angles_to_pixels(lib, data->lat, data->lon, data->delta1);
  float pixel_lat = pixel_lon * data->delta2 * data->ratio / data->delta1;
  GdkPixbuf *draw = NULL;
  if(data->shape == MAP_LOCATION_SHAPE_ELLIPSE)
  {
    draw = _draw_ellipse(pixel_lon, pixel_lat, main);
    if(pixel_lon > pixel_lat)
      pixel_lat = pixel_lon;
    else
      pixel_lon = pixel_lat;
  }
  else if(data->shape == MAP_LOCATION_SHAPE_RECTANGLE)
    draw = _draw_rectangle(pixel_lon, pixel_lat, main);

  if(width) *width = (int)pixel_lon;
  if(height) *height = (int)pixel_lat;
  return draw;
}

dt_location_draw_t *_others_location_draw(dt_map_t *lib, const int locid)
{
  for(GList *other = lib->loc.others; other; other = g_list_next(other))
  {
    dt_location_draw_t *d = (dt_location_draw_t *)other->data;
    if(d->id == locid)
      return d;
  }
  return NULL;
}

GList *_others_location(GList *others, const int locid)
{
  for(GList *other = others; other; other = g_list_next(other))
  {
    dt_location_draw_t *d = (dt_location_draw_t *)other->data;
    if(d->id == locid)
      return other;
  }
  return NULL;
}

static OsmGpsMapImage *_view_map_draw_location(dt_map_t *lib, dt_location_draw_t *ld,
                                               const gboolean main)
{
  if(ld->data.shape != MAP_LOCATION_SHAPE_POLYGONS)
  {
    GdkPixbuf *draw = _draw_location(lib, NULL, NULL, &ld->data, main);
    OsmGpsMapImage *location = NULL;
    if(draw)
    {
      location = osm_gps_map_image_add_with_alignment(lib->map, ld->data.lat, ld->data.lon, draw, 0.5, 0.5);
      g_object_unref(draw);
    }
    return location;
  }
  else
  {
    if(!ld->data.polygons && ld == &lib->loc.main)
    {
      dt_location_draw_t *d = _others_location_draw(lib, lib->loc.main.id);
      if(d)
      {
        ld->data.polygons = d->data.polygons;
        ld->data.plg_pts = d->data.plg_pts;
      }
    }
    if(!ld->data.polygons)
      dt_map_location_get_polygons(ld);
    OsmGpsMapPolygon *location = _view_map_add_polygon_location(lib, ld);
    return (OsmGpsMapImage *)location;
  }
}

static void _view_map_remove_location(dt_map_t *lib, dt_location_draw_t *ld)
{
  if(ld->location)
  {
    if(ld->data.shape != MAP_LOCATION_SHAPE_POLYGONS)
      osm_gps_map_image_remove(lib->map, ld->location);
    else
      osm_gps_map_polygon_remove(lib->map, (OsmGpsMapPolygon *)ld->location);
    ld->location = NULL;
  }
}

static void _view_map_delete_other_location(dt_map_t *lib, GList *other)
{
  dt_location_draw_t *d = (dt_location_draw_t *)other->data;
  _view_map_remove_location(lib, d);
  dt_map_location_free_polygons(d);
  lib->loc.others = g_list_remove_link(lib->loc.others, other);
  g_free(other->data);
  g_list_free(other);
}

static void _view_map_draw_main_location(dt_map_t *lib, dt_location_draw_t *ld)
{
  if(lib->loc.main.id && (ld != &lib->loc.main))
  {
    // save the data to others locations (including polygons)
    dt_location_draw_t *d = _others_location_draw(lib, lib->loc.main.id);
    if(!d)
    {
      d = g_malloc0(sizeof(dt_location_draw_t));
      lib->loc.others = g_list_append(lib->loc.others, d);
    }
    if(d)
    {
      // copy everything except the drawn location
      memcpy(d, &lib->loc.main, sizeof(dt_location_draw_t));
      d->location = NULL;
      if(dt_conf_get_bool("plugins/map/showalllocations"))
        d->location = _view_map_draw_location(lib, d, FALSE);
    }
  }
  _view_map_remove_location(lib, &lib->loc.main);

  if(ld && ld->id)
  {
    memcpy(&lib->loc.main, ld, sizeof(dt_location_draw_t));
    lib->loc.main.location = _view_map_draw_location(lib, &lib->loc.main, TRUE);
    dt_location_draw_t *d = _others_location_draw(lib, ld->id);
    if(d && d->location)
      _view_map_remove_location(lib, d);
  }
}

static void _view_map_draw_other_locations(dt_map_t *lib, dt_map_box_t *bbox)
{
  // clean up other locations
  {
    for(GList *other = lib->loc.others; other; other = g_list_next(other))
    {
      dt_location_draw_t *d = (dt_location_draw_t *)other->data;
      _view_map_remove_location(lib, d);
    }
  }
  if(dt_conf_get_bool("plugins/map/showalllocations"))
  {
    GList *others = dt_map_location_get_locations_on_map(bbox);

    for(GList *other = others; other; other = g_list_next(other))
    {
      dt_location_draw_t *d = (dt_location_draw_t *)other->data;
      GList *other2 = _others_location(lib->loc.others, d->id);
      // add the new ones
      if(!other2)
      {
        d = (dt_location_draw_t *)other->data;
        lib->loc.others = g_list_append(lib->loc.others, other->data);
        other->data = NULL; // to avoid it gets freed
        if(d->data.shape == MAP_LOCATION_SHAPE_POLYGONS)
        {
          if(d->id == lib->loc.main.id)
          {
            d->data.polygons = lib->loc.main.data.polygons;
            d->data.plg_pts = lib->loc.main.data.plg_pts;
          }
          if(!d->data.polygons)
            dt_map_location_get_polygons(d);
        }
      }
      else
        d = (dt_location_draw_t *)other2->data;
      // display again location except polygons and the selected one
      if((!lib->loc.main.id || lib->loc.main.id != d->id) && !d->location)
        d->location = _view_map_draw_location(lib, d, FALSE);
    }
    // free the new list
    g_list_free_full(others, g_free);
  }
}

static void _view_map_update_location_geotag(dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  if(lib->loc.main.id > 0)
  {
    // update coordinates
    dt_map_location_set_data(lib->loc.main.id, &lib->loc.main.data);
    if(dt_map_location_update_images(&lib->loc.main))
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  }
}

static GdkPixbuf *_draw_image(const dt_imgid_t imgid, int *width, int *height,
                              const int group_count, const gboolean group_same_loc,
                              const uint32_t frame, const gboolean blocking,
                              const int thumbnail, dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;

  GdkPixbuf *thumb = NULL, *source = NULL, *count = NULL;
  const int _thumb_size = DT_PIXEL_APPLY_DPI(thumb_size);
  const float _thumb_border = DT_PIXEL_APPLY_DPI(thumb_border);
  const float _pin_size = DT_PIXEL_APPLY_DPI(image_pin_size);

  if(thumbnail == DT_MAP_THUMB_THUMB)
  {
    dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, _thumb_size, _thumb_size);
    dt_mipmap_buffer_t buf;
    dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, mip,
                        blocking ? DT_MIPMAP_BLOCKING : DT_MIPMAP_BEST_EFFORT, 'r');

    if(buf.buf && buf.width > 0)
    {
      for(int i = 3; i < (size_t)4 * buf.width * buf.height; i += 4) buf.buf[i] = UINT8_MAX;

      int w = _thumb_size, h = _thumb_size;
      if(buf.width < buf.height)
        w = (buf.width * _thumb_size) / buf.height; // portrait
      else
        h = (buf.height * _thumb_size) / buf.width; // landscape

      // next we get a pixbuf for the image
      source = gdk_pixbuf_new_from_data(buf.buf, GDK_COLORSPACE_RGB, TRUE,
                                        8, buf.width, buf.height,
                                        buf.width * 4, NULL, NULL);
      dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
      if(!source) goto map_changed_failure;
      // now we want a slightly larger pixbuf that we can put the image on
      thumb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w + 2 * _thumb_border,
                             h + 2 * _thumb_border + _pin_size);
      if(!thumb) goto map_changed_failure;
      gdk_pixbuf_fill(thumb, frame);
      // put the image onto the frame
      gdk_pixbuf_scale(source, thumb, _thumb_border, _thumb_border, w, h,
                       _thumb_border, _thumb_border, (1.0 * w) / buf.width,
                       (1.0 * h) / buf.height, GDK_INTERP_HYPER);
      // add the pin
      gdk_pixbuf_copy_area(lib->image_pin, 0, 0, w + 2 * _thumb_border,
                           _pin_size, thumb, 0, h + 2 * _thumb_border);
      // add the count
      if(group_count)
      {
        double count_height, count_width;
        count = _view_map_images_count(group_count, group_same_loc,
                                       &count_width, &count_height);
        gdk_pixbuf_copy_area(count, 0, 0, count_width, count_height, thumb,
                            _thumb_border, h - count_height + _thumb_border);

      }
      if(width) *width = w;
      if(height) *height = h;
    }
  }
  else if(thumbnail == DT_MAP_THUMB_COUNT)
  {
    double count_height, count_width;
    count = _view_map_images_count(group_count, group_same_loc,
                                   &count_width, &count_height);
    if(!count) goto map_changed_failure;
    const int w = count_width + 2 * _thumb_border;
    const int h = count_height + 2 * _thumb_border + _pin_size;
    thumb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w, h);
    if(!thumb) goto map_changed_failure;
    gdk_pixbuf_fill(thumb, frame);
    gdk_pixbuf_copy_area(count, 0, 0, count_width, count_height, thumb,
                       _thumb_border, _thumb_border);
    gdk_pixbuf_copy_area(lib->image_pin, 0, 0, w, _pin_size, thumb,
                       0, count_height + 2 * _thumb_border);
    if(width) *width = count_width;
    if(height) *height = count_height;
  }
map_changed_failure:
  if(source) g_object_unref(source);
  if(count) g_object_unref(count);
  return thumb;
}

static gboolean _view_map_draw_image(dt_map_image_t *entry, const gboolean blocking,
                                     const int thumbnail, dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  gboolean needs_redraw = FALSE;
  if(entry->image && entry->thumbnail != thumbnail)
  {
    osm_gps_map_image_remove(lib->map, entry->image);
    entry->image = NULL;
  }
  if(!entry->image)
  {
    GdkPixbuf *thumb = _draw_image(entry->imgid, &entry->width, &entry->height,
                                   entry->group_count, entry->group_same_loc,
                                   entry->selected_in_group ? thumb_frame_sel_color : thumb_frame_color,
                                   blocking, thumbnail, self);
    if(thumb)
    {
      entry->thumbnail = thumbnail;
      entry->image = osm_gps_map_image_add_with_alignment(lib->map, entry->latitude,
                                                          entry->longitude,
                                                          thumb, 0, 1);
      g_object_unref(thumb);
    }
    else
      needs_redraw = TRUE;
  }
  return needs_redraw;
}

// scan the images list and draw the missing ones
// if launched to be executed repeatedly, return FALSE when it is done
static gboolean _view_map_draw_images(gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_map_t *lib = (dt_map_t *)self->data;
  gboolean needs_redraw = FALSE;
  int img_drawn = 0;
  for(GSList *iter = lib->images; iter; iter = g_slist_next(iter))
  {
    needs_redraw = _view_map_draw_image((dt_map_image_t *)iter->data, FALSE,
                                        lib->thumbnail, self);
    img_drawn++;
    // we limit the number of displayed images as required
    if(lib->thumbnail == DT_MAP_THUMB_THUMB && img_drawn >= lib->max_images_drawn)
      break;
  }
  if(!needs_redraw)
    lib->timeout_event_source = 0;
  return needs_redraw;
}

static void _view_map_get_bounding_box(dt_map_t *lib, dt_map_box_t *bbox)
{
  OsmGpsMapPoint bb[2];
  osm_gps_map_get_bbox(lib->map, &bb[0], &bb[1]);
  dt_map_box_t box = {0.0};

  osm_gps_map_point_get_degrees(&bb[0], &box.lat1, &box.lon1);
  osm_gps_map_point_get_degrees(&bb[1], &box.lat2, &box.lon2);
  box.lat1 = CLAMP(box.lat1, -90.0, 90.0);
  box.lat2 = CLAMP(box.lat2, -90.0, 90.0);
  box.lon1 = CLAMP(box.lon1, -180.0, 180.0);
  box.lon2 = CLAMP(box.lon2, -180.0, 180.0);
  memcpy(bbox, &box, sizeof(dt_map_box_t));
}

static void _view_map_changed_callback_delayed(gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_map_t *lib = (dt_map_t *)self->data;
  gboolean all_good = TRUE;
  gboolean needs_redraw = FALSE;
  gboolean prefs_changed = _view_map_prefs_changed(lib);

  if(!lib->timeout_event_source)
  {
    /* remove the old images */
    if(lib->images)
    {
      // we can't use osm_gps_map_image_remove_all() because we want to keep the marker
      for(GSList *iter = lib->images; iter; iter = g_slist_next(iter))
      {
        dt_map_image_t *image = (dt_map_image_t *)iter->data;
        if(image->image)
          osm_gps_map_image_remove(lib->map, image->image);
      }
      g_slist_free_full(lib->images, g_free);
      lib->images = NULL;
    }
  }

  if(!lib->timeout_event_source && lib->thumbnail != DT_MAP_THUMB_NONE)
  {
    // not a redraw
    // rebuild main_query if needed
    if(prefs_changed) _view_map_build_main_query(lib);

    /* get bounding box coords */
    _view_map_get_bounding_box(lib, &lib->bbox);

    /* get map view state and store  */
    int zoom;
    float center_lat, center_lon;
    g_object_get(G_OBJECT(lib->map), "zoom", &zoom, "latitude", &center_lat, "longitude", &center_lon, NULL);
    dt_conf_set_float("plugins/map/longitude", center_lon);
    dt_conf_set_float("plugins/map/latitude", center_lat);
    dt_conf_set_int("plugins/map/zoom", zoom);

    /* let's reset and reuse the main_query statement */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->main_query);
    DT_DEBUG_SQLITE3_RESET(lib->main_query);

    /* bind bounding box coords for the main query */
    DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->main_query, 1, lib->bbox.lon1);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->main_query, 2, lib->bbox.lon2);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->main_query, 3, lib->bbox.lat1);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->main_query, 4, lib->bbox.lat2);

    // count the images
    int img_count = 0;
    while(sqlite3_step(lib->main_query) == SQLITE_ROW)
    {
      img_count++;
    }

    if(lib->points)
      g_free(lib->points);
    lib->points = NULL;
    lib->nb_points = img_count;
    if(img_count > 0)
      lib->points = (dt_geo_position_t *)calloc(img_count, sizeof(dt_geo_position_t));
    dt_geo_position_t *p = lib->points;
    if(p)
    {
      DT_DEBUG_SQLITE3_RESET(lib->main_query);
      /* make the image list */
      int i = 0;
      while(sqlite3_step(lib->main_query) == SQLITE_ROW && all_good && i < img_count)
      {
        p[i].imgid = sqlite3_column_int(lib->main_query, 0);
        p[i].x = sqlite3_column_double(lib->main_query, 1) * M_PI / 180;
        p[i].y = sqlite3_column_double(lib->main_query, 2) * M_PI / 180;
        p[i].cluster_id = UNCLASSIFIED;
        i++;
      }

      const float epsilon_factor = dt_conf_get_int("plugins/map/epsilon_factor");
      const int min_images = dt_conf_get_int("plugins/map/min_images_per_group");
      // zoom varies from 0 (156412 m/pixel) to 20 (0.149 m/pixel)
      // https://wiki.openstreetmap.org/wiki/Zoom_levels
      // each time zoom increases by 1 the size is divided by 2
      // epsilon factor = 100 => epsilon covers more or less a thumbnail surface
      #define R 6371   // earth radius (km)
      double epsilon = thumb_size * (((unsigned int)(156412000 >> zoom))
                                  * epsilon_factor * 0.01 * 0.000001 / R);

      dt_times_t start;
      dt_get_times(&start);
      _dbscan(p, img_count, epsilon, min_images);
      dt_show_times(&start, "[map] dbscan calculation");

      // set the clusters
      GList *sel_imgs = dt_act_on_get_images(FALSE, FALSE, FALSE);
      int group = -1;
      for(i = 0; i< img_count; i++)
      {
        if(p[i].cluster_id == NOISE)
        {
          dt_map_image_t *entry = (dt_map_image_t *)calloc(1, sizeof(dt_map_image_t));
          entry->imgid = p[i].imgid;
          entry->group = p[i].cluster_id;
          entry->group_count = 1;
          entry->longitude = p[i].x * 180 / M_PI;
          entry->latitude = p[i].y * 180 / M_PI;
          entry->group_same_loc = TRUE;
          if(sel_imgs)
            entry->selected_in_group = g_list_find((GList *)sel_imgs,
                                                   GINT_TO_POINTER(entry->imgid))
                                       ? TRUE : FALSE;
          lib->images = g_slist_prepend(lib->images, entry);
        }
        else if(p[i].cluster_id > group)
        {
          group = p[i].cluster_id;
          dt_map_image_t *entry = (dt_map_image_t *)calloc(1, sizeof(dt_map_image_t));
          entry->imgid = p[i].imgid;
          entry->group = p[i].cluster_id;
          entry->group_same_loc = TRUE;
          entry->selected_in_group = (sel_imgs && g_list_find((GList *)sel_imgs,
                                                               GINT_TO_POINTER(p[i].imgid)))
                                     ? TRUE : FALSE;
          const double lon = p[i].x, lat = p[i].y;
          for(int j = 0; j < img_count; j++)
          {
            if(p[j].cluster_id == group)
            {
              entry->group_count++;
              entry->longitude += p[j].x;
              entry->latitude += p[j].y;
              if(entry->group_same_loc && (p[j].x != lon || p[j].y != lat))
              {
                entry->group_same_loc = FALSE;
              }
              if(sel_imgs && !entry->selected_in_group)
              {
                if(g_list_find((GList *)sel_imgs, GINT_TO_POINTER(p[j].imgid)))
                  entry->selected_in_group = TRUE;
              }
            }
          }
          entry->latitude = entry->latitude  * 180 / M_PI / entry->group_count;
          entry->longitude = entry->longitude * 180 / M_PI / entry->group_count;
          lib->images = g_slist_prepend(lib->images, entry);
        }
      }
      g_list_free(sel_imgs);
    }

    needs_redraw = _view_map_draw_images(self);
    _view_map_draw_main_location(lib, &lib->loc.main);
    _view_map_draw_other_locations(lib, &lib->bbox);
  }

  // not exactly thread safe, but should be good enough for updating the display
  if(needs_redraw && lib->timeout_event_source == 0)
  {
    lib->timeout_event_source = g_timeout_add(100, _view_map_draw_images, self); // try again later on
  }
}

static gboolean _view_map_changed_callback_wait(gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_map_t *lib = (dt_map_t *)self->data;
  if(lib->time_out)
  {
    lib->time_out--;
    if(!lib->time_out)
    {
      _view_map_changed_callback_delayed(self);
      return FALSE;
    }
  }
  return TRUE;
}

static int first_times = 3;

static void _view_map_changed_callback(OsmGpsMap *map, dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  // ugly but it avoids to display not well controlled maps at init time
  if(first_times)
  {
    first_times--;;
    return;
  }

  // "changed" event can be high frequency. As calculation is heavy we don't to repeat it.
  if(!lib->time_out)
  {
    g_timeout_add(100, _view_map_changed_callback_wait, self);
  }
  lib->time_out = 2;

  // activate this callback late in the process as we need the filmstrip proxy to be setup. This is not the
  // case in the initialization phase.
  if(!lib->drop_filmstrip_activated)
  {
    g_signal_connect(dt_ui_thumbtable(darktable.gui->ui)->widget, "drag-data-received",
                     G_CALLBACK(_view_map_dnd_remove_callback), self);
    lib->drop_filmstrip_activated = TRUE;
  }
}

static dt_map_image_t *_view_map_get_entry_at_pos(dt_view_t *self, const double x,
                                                  const double y)
{
  dt_map_t *lib = (dt_map_t *)self->data;

  for(const GSList *iter = lib->images; iter; iter = g_slist_next(iter))
  {
    dt_map_image_t *entry = (dt_map_image_t *)iter->data;
    OsmGpsMapImage *image = entry->image;
    if(image)
    {
      OsmGpsMapPoint *pt = (OsmGpsMapPoint *)osm_gps_map_image_get_point(image);
      gint img_x = 0, img_y = 0;
      osm_gps_map_convert_geographic_to_screen(lib->map, pt, &img_x, &img_y);
      img_y -= DT_PIXEL_APPLY_DPI(image_pin_size);
      if(x >= img_x && x <= img_x + entry->width && y <= img_y && y >= img_y - entry->height)
      {
        return entry;
      }
    }
  }
  return NULL;
}

static GList *_view_map_get_imgs_at_pos(dt_view_t *self, const float x, const float y,
                                        int *offset_x, int *offset_y, const gboolean first_on)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  GList *imgs = NULL;
  dt_imgid_t imgid = NO_IMGID;
  dt_map_image_t *entry = NULL;

  for(const GSList *iter = lib->images; iter; iter = g_slist_next(iter))
  {
    entry = (dt_map_image_t *)iter->data;
    OsmGpsMapImage *image = entry->image;
    if(image)
    {
      OsmGpsMapPoint *pt = (OsmGpsMapPoint *)osm_gps_map_image_get_point(image);
      gint img_x = 0, img_y = 0;
      osm_gps_map_convert_geographic_to_screen(lib->map, pt, &img_x, &img_y);
      img_y -= DT_PIXEL_APPLY_DPI(image_pin_size);
      if(x >= img_x && x <= img_x + entry->width && y <= img_y && y >= img_y - entry->height)
      {
        imgid = entry->imgid;
        *offset_x = (int)x - img_x;
        *offset_y = (int)y - img_y - DT_PIXEL_APPLY_DPI(image_pin_size);
        break;
      }
    }
  }

  if(dt_is_valid_imgid(imgid) && !first_on && entry->group_count > 1 && lib->points)
  {
    dt_geo_position_t *p = lib->points;
    int count = 1;
    for(int i = 0; i < lib->nb_points; i++)
    {
      if(p[i].cluster_id == entry->group && p[i].imgid != imgid)
      {
        imgs = g_list_prepend(imgs, GINT_TO_POINTER(p[i].imgid));
        count++;
        if(count >= entry->group_count)
        {
          break;
        }
      }
    }
  }
  if(dt_is_valid_imgid(imgid))
    // it's necessary to have the visible image as the first one of the list
    imgs = g_list_prepend(imgs, GINT_TO_POINTER(imgid));
  return imgs;
}

gint _find_image_in_images(gconstpointer a, gconstpointer b)
{
  dt_map_image_t *entry = (dt_map_image_t *)a;
  return entry->imgid == GPOINTER_TO_INT(b) ? 0 : 1;
}

static gboolean _display_next_image(dt_view_t *self, dt_map_image_t *entry, const gboolean next)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  if(!entry) return FALSE;

  if(entry->group_count == 1)
  {
    if(entry->image)
    {
      osm_gps_map_image_remove(lib->map, entry->image);
      entry->image = NULL;
    }
    _view_map_draw_image(entry, TRUE, DT_MAP_THUMB_THUMB, self);
    dt_control_set_mouse_over_id(entry->imgid);
    return TRUE;
  }

  dt_geo_position_t *p = lib->points;
  int index = -1;
  for(int i = 0; i < lib->nb_points; i++)
  {
    if(p[i].imgid == GPOINTER_TO_INT(entry->imgid))
    {
      if(next)
      {
        for(int j = i + 1; j < lib->nb_points; j++)
        {
          if(p[j].cluster_id == entry->group)
          {
            index = j;
            break;
          }
        }
        if(index == -1)
        {
          for(int j = 0; j < i; j++)
          {
            if(p[j].cluster_id == entry->group)
            {
              index = j;
              break;
            }
          }
        }
      }
      else
      {
        for(int j = i - 1; j >= 0; j--)
        {
          if(p[j].cluster_id == entry->group)
          {
            index = j;
            break;
          }
        }
        if(index == -1)
        {
          for(int j = lib->nb_points - 1; j > i; j--)
          {
            if(p[j].cluster_id == entry->group)
            {
              index = j;
              break;
            }
          }
        }
      }
      break;
    }
  }
  if(index == -1) return FALSE;
  entry->imgid = p[index].imgid;
  if(entry->image)
  {
    osm_gps_map_image_remove(lib->map, entry->image);
    entry->image = NULL;
  }
  _view_map_draw_image(entry, TRUE, DT_MAP_THUMB_THUMB, self);
  dt_control_set_mouse_over_id(entry->imgid);
  return TRUE;
}

static void _view_map_drag_set_icon(const dt_view_t *self, GdkDragContext *context,
                                    const dt_imgid_t imgid, const int count)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  int height;
  GdkPixbuf *thumb = _draw_image(imgid, NULL, &height, count, TRUE, thumb_frame_sel_color,
                                 TRUE, lib->thumbnail, (dt_view_t *)self);
  if(thumb)
  {
    GtkWidget *image = gtk_image_new_from_pixbuf(thumb);
    dt_gui_add_class(image, "dt_transparent_background");
    gtk_widget_show(image);
    gtk_drag_set_icon_widget(context, image, lib->start_drag_offset_x,
                             DT_PIXEL_APPLY_DPI(height + image_pin_size + 2 * thumb_border) + lib->start_drag_offset_y);
    g_object_unref(thumb);
  }
}

static gboolean _view_map_drag_motion_callback(GtkWidget *widget, GdkDragContext *dc,
                                               gint x, gint y, guint time, dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  OsmGpsMapPoint *p = osm_gps_map_point_new_degrees(0.0, 0.0);
  osm_gps_map_convert_screen_to_geographic(lib->map, x, y, p);
  float lat, lon;
  osm_gps_map_point_get_degrees(p, &lat, &lon);
  osm_gps_map_point_free(p);
  _toast_log_lat_lon(lat, lon);
  return FALSE;
}

static gboolean _view_map_motion_notify_callback(GtkWidget *widget, GdkEventMotion *e, dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  OsmGpsMapPoint *p = osm_gps_map_get_event_location(lib->map, (GdkEventButton *)e);
  float lat, lon;
  osm_gps_map_point_get_degrees(p, &lat, &lon);
  _toast_log_lat_lon(lat, lon);

  if(lib->loc.drag && lib->loc.main.id > 0 &&
     (abs(lib->start_drag_x - (int)ceil(e->x_root)) +
      abs(lib->start_drag_y - (int)ceil(e->y_root))) > DT_PIXEL_APPLY_DPI(8))
  {
    lib->loc.drag = FALSE;
    osm_gps_map_image_remove(lib->map, lib->loc.main.location);
    GtkTargetList *targets = gtk_target_list_new(target_list_internal, n_targets_internal);

    GdkDragContext *context =
      gtk_drag_begin_with_coordinates(GTK_WIDGET(lib->map), targets,
                                      GDK_ACTION_MOVE, 1,
                                      (GdkEvent *)e, -1, -1);

    int width;
    int height;
    GdkPixbuf *location = _draw_location(lib, &width, &height, &lib->loc.main.data, TRUE);
    if(location)
    {
      GtkWidget *image = gtk_image_new_from_pixbuf(location);
      gtk_widget_set_name(image, "map-drag-icon");
      gtk_widget_show(image);
      gtk_drag_set_icon_widget(context, image,
                               DT_PIXEL_APPLY_DPI(width),
                               DT_PIXEL_APPLY_DPI(height));
      g_object_unref(location);
    }
    gtk_target_list_unref(targets);
    return TRUE;
  }

  if(lib->start_drag && lib->selected_images &&
     (abs(lib->start_drag_x - (int)ceil(e->x_root)) +
      abs(lib->start_drag_y - (int)ceil(e->y_root))) > DT_PIXEL_APPLY_DPI(8))
  {
    const int nb = g_list_length(lib->selected_images);
    for(const GSList *iter = lib->images; iter; iter = g_slist_next(iter))
    {
      dt_map_image_t *entry = (dt_map_image_t *)iter->data;
      if(entry->image)
      {
        GList *sel_img = lib->selected_images;
        if(entry->imgid == GPOINTER_TO_INT(sel_img->data))
        {
          if(entry->group_count == nb)
          {
            osm_gps_map_image_remove(lib->map, entry->image);
            entry->image = NULL;
          }
          else
            _display_next_image(self, entry, TRUE);
          break;
        }
      }
    }

    const int group_count = g_list_length(lib->selected_images);

    lib->start_drag = FALSE;
    GtkTargetList *targets = gtk_target_list_new(target_list_all, n_targets_all);
    GdkDragContext *context = gtk_drag_begin_with_coordinates(GTK_WIDGET(lib->map), targets,
                                                              GDK_ACTION_MOVE, 1,
                                                              (GdkEvent *)e, -1, -1);
    _view_map_drag_set_icon(self, context, GPOINTER_TO_INT(lib->selected_images->data),
                            group_count);
    gtk_target_list_unref(targets);
    return TRUE;
  }


  dt_map_image_t *entry = _view_map_get_entry_at_pos(self, e->x, e->y);
  if(entry)
  {
    // show image information if image is hovered
    dt_control_set_mouse_over_id(entry->imgid);
    // if count is displayed shows the thumbnail
    if(lib->thumbnail == DT_MAP_THUMB_COUNT)
    {
      _view_map_draw_image(entry, TRUE, DT_MAP_THUMB_THUMB, self);
      lib->last_hovered_entry = entry;
      return TRUE;
    }
  }
  else
  {
    dt_control_set_mouse_over_id(NO_IMGID);
    if(lib->last_hovered_entry)
    {
      // _view_map_draw_image(lib->last_hovered_entry) would be faster but is not safe
      _view_map_draw_images(self);
      lib->last_hovered_entry = NULL;
    }
  }
  return FALSE;
}

static gboolean _zoom_and_center(const gint x, const gint y, const int direction, dt_view_t *self)
{
    // try to keep the center of zoom at the mouse position
  dt_map_t *lib = (dt_map_t *)self->data;
  int zoom, max_zoom;
  g_object_get(G_OBJECT(lib->map), "zoom", &zoom, "max-zoom", &max_zoom, NULL);

  OsmGpsMapPoint bb[2];
  osm_gps_map_get_bbox(lib->map, &bb[0], &bb[1]);
  gint x0, x1, y0, y1;
  osm_gps_map_convert_geographic_to_screen(lib->map, &bb[0], &x0, &y0);
  osm_gps_map_convert_geographic_to_screen(lib->map, &bb[1], &x1, &y1);

  gint nx, ny;
  if(direction == GDK_SCROLL_UP && zoom < max_zoom)
  {
    zoom++;
    nx = (x0 + x1 + 2 * x) / 4;
    ny = (y0 + y1 + 2 * y) / 4;
  }
  else if(direction == GDK_SCROLL_DOWN && zoom  > 0)
  {
    zoom--;
    nx = x0 + x1 - x;
    ny = y0 + y1 - y;
  }
  else return FALSE;

  OsmGpsMapPoint *pt = osm_gps_map_point_new_degrees(0.0, 0.0);
  osm_gps_map_convert_screen_to_geographic(lib->map, nx, ny, pt);
  float nlat, nlon;
  osm_gps_map_point_get_degrees(pt, &nlat, &nlon);
  osm_gps_map_point_free(pt);
  osm_gps_map_set_center_and_zoom(lib->map, nlat, nlon, zoom);

  return TRUE;
}

static gboolean _view_map_scroll_event(GtkWidget *w, GdkEventScroll *event, dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  // check if the click was on image(s) or just some random position
  dt_map_image_t *entry = _view_map_get_entry_at_pos(self, event->x, event->y);
  if(entry)
  {
    if(_display_next_image(self, entry, event->direction == GDK_SCROLL_DOWN))
      return TRUE;
  }

  OsmGpsMapPoint *p = osm_gps_map_get_event_location(lib->map, (GdkEventButton *)event);
  float lat, lon;
  osm_gps_map_point_get_degrees(p, &lat, &lon);
  if(lib->loc.main.id > 0)
  {
    if(dt_map_location_included(lon, lat, &lib->loc.main.data))
    {
      if(dt_modifier_is(event->state, GDK_SHIFT_MASK))
      {
        if(event->direction == GDK_SCROLL_DOWN)
          lib->loc.main.data.delta1 *= 1.1;
        else
          lib->loc.main.data.delta1 /= 1.1;
      }
      else if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
      {
        if(event->direction == GDK_SCROLL_DOWN)
          lib->loc.main.data.delta2 *= 1.1;
        else
          lib->loc.main.data.delta2 /= 1.1;
      }
      else
      {
        if(event->direction == GDK_SCROLL_DOWN)
        {
          lib->loc.main.data.delta1 *= 1.1;
          lib->loc.main.data.delta2 *= 1.1;
        }
        else
        {
          lib->loc.main.data.delta1 /= 1.1;
          lib->loc.main.data.delta2 /= 1.1;
        }
      }
      _view_map_draw_main_location(lib, &lib->loc.main);
      _view_map_update_location_geotag(self);
      _view_map_signal_change_wait(self, 5);  // wait 5/10 sec after last scroll
      return TRUE;
    }
    else  // scroll on the map. try to keep the map where it is
    {
      _toast_log_lat_lon(lat, lon);
      return _zoom_and_center(event->x, event->y, event->direction, self);
    }
  }
  else
  {
    _toast_log_lat_lon(lat, lon);
    return _zoom_and_center(event->x, event->y, event->direction, self);
  }
  return FALSE;
}

static gboolean _view_map_button_press_callback(GtkWidget *w, GdkEventButton *e, dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  if(lib->selected_images)
  {
    g_list_free(lib->selected_images);
    lib->selected_images = NULL;
  }
  if(e->button == 1)
  {
    // check if the click was in a location form - crtl gives priority to images
    if(lib->loc.main.id > 0 && (lib->loc.main.data.shape != MAP_LOCATION_SHAPE_POLYGONS)
                            && !dt_modifier_is(e->state, GDK_CONTROL_MASK))
    {

      OsmGpsMapPoint *p = osm_gps_map_get_event_location(lib->map, e);
      float lat, lon;
      osm_gps_map_point_get_degrees(p, &lat, &lon);
      if(dt_map_location_included(lon, lat, &lib->loc.main.data))
      {
        if(!dt_modifier_is(e->state, GDK_SHIFT_MASK))
        {
          lib->start_drag_x = ceil(e->x_root);
          lib->start_drag_y = ceil(e->y_root);
          lib->loc.drag = TRUE;
          return TRUE;
        }
      }
    }
    // check if another location is clicked - ctrl gives priority to images
    if(!dt_modifier_is(e->state, GDK_CONTROL_MASK) &&
        dt_conf_get_bool("plugins/map/showalllocations"))
    {
      OsmGpsMapPoint *p = osm_gps_map_get_event_location(lib->map, e);
      float lat, lon;
      osm_gps_map_point_get_degrees(p, &lat, &lon);
      for(GList *other = lib->loc.others; other; other = g_list_next(other))
      {
        dt_location_draw_t *d = (dt_location_draw_t *)other->data;
        if(dt_map_location_included(lon, lat, &d->data))
        {
          dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_view_map_geotag_changed), self);
          dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_view_map_collection_changed), self);
          DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_GEOTAG_CHANGED, (GList *)NULL, d->id);
          dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_view_map_collection_changed), self);
          dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_view_map_geotag_changed), self);
          return TRUE;
        }
      }
    }
    // check if the click was on image(s) or just some random position
    lib->selected_images = _view_map_get_imgs_at_pos(self, e->x, e->y,
                                                     &lib->start_drag_offset_x, &lib->start_drag_offset_y,
                                                     !dt_modifier_is(e->state, GDK_SHIFT_MASK));
    if(e->type == GDK_BUTTON_PRESS)
    {
      if(lib->selected_images)
      {
        lib->start_drag_x = ceil(e->x_root);
        lib->start_drag_y = ceil(e->y_root);
        lib->start_drag = TRUE;
        return TRUE;
      }
      else
      {
        return FALSE;
      }
    }
    if(e->type == GDK_2BUTTON_PRESS)
    {
      if(lib->selected_images)
      {
        // open the image in darkroom
        dt_control_set_mouse_over_id(GPOINTER_TO_INT(lib->selected_images->data));
        dt_ctl_switch_mode_to("darkroom");
        return TRUE;
      }
      else
      {
        // zoom into that position
        float longitude, latitude;
        OsmGpsMapPoint *pt = osm_gps_map_point_new_degrees(0.0, 0.0);
        osm_gps_map_convert_screen_to_geographic(lib->map, e->x, e->y, pt);
        osm_gps_map_point_get_degrees(pt, &latitude, &longitude);
        osm_gps_map_point_free(pt);
        int zoom, max_zoom;
        g_object_get(G_OBJECT(lib->map), "zoom", &zoom, "max-zoom", &max_zoom, NULL);
        zoom = MIN(zoom + 1, max_zoom);
        _view_map_center_on_location(self, longitude, latitude, zoom);
      }
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean _view_map_button_release_callback(GtkWidget *w, GdkEventButton *e, dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  lib->start_drag = FALSE;
  lib->start_drag_offset_x = 0;
  lib->start_drag_offset_y = 0;
  lib->loc.drag = FALSE;
  return FALSE;
}

static gboolean _view_map_display_selected(gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_map_t *lib = (dt_map_t *)self->data;
  gboolean done = FALSE;
  // selected images ?
  done = _view_map_center_on_image_list(self, "main.selected_images");

  // collection ?
  if(!done)
  {
    done = _view_map_center_on_image_list(self, "memory.collected_images");
  }

  // last map view
  if(!done)
  {
    /* if nothing to show restore last zoom,location in map */
    float lon = dt_conf_get_float("plugins/map/longitude");
    lon = CLAMP(lon, -180, 180);
    float lat = dt_conf_get_float("plugins/map/latitude");
    lat = CLAMP(lat, -90, 90);
    const int zoom = dt_conf_get_int("plugins/map/zoom");
    osm_gps_map_set_center_and_zoom(lib->map, lat, lon, zoom);
  }
  return FALSE; // don't call again
}

void enter(dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;

  lib->selected_images = NULL;
  lib->start_drag = FALSE;
  lib->start_drag_offset_x = 0;
  lib->start_drag_offset_y = 0;
  lib->loc.drag = FALSE;
  lib->entering = TRUE;

  /* set the correct map source */
  _view_map_set_map_source_g_object(self, lib->map_source);

  /* add map to center widget */
  gtk_overlay_add_overlay(GTK_OVERLAY(dt_ui_center_base(darktable.gui->ui)), GTK_WIDGET(lib->map));

  // ensure the log msg widget stay on top
  gtk_overlay_reorder_overlay(GTK_OVERLAY(dt_ui_center_base(darktable.gui->ui)),
                              gtk_widget_get_parent(dt_ui_log_msg(darktable.gui->ui)), -1);
  gtk_overlay_reorder_overlay(GTK_OVERLAY(dt_ui_center_base(darktable.gui->ui)),
                              gtk_widget_get_parent(dt_ui_toast_msg(darktable.gui->ui)), -1);

  gtk_widget_show_all(GTK_WIDGET(lib->map));

  /* setup proxy functions */
  darktable.view_manager->proxy.map.center_on_location = _view_map_center_on_location;
  darktable.view_manager->proxy.map.center_on_bbox = _view_map_center_on_bbox;
  darktable.view_manager->proxy.map.show_osd = _view_map_show_osd;
  darktable.view_manager->proxy.map.set_map_source = _view_map_set_map_source;
  darktable.view_manager->proxy.map.add_marker = _view_map_add_marker;
  darktable.view_manager->proxy.map.remove_marker = _view_map_remove_marker;
  darktable.view_manager->proxy.map.add_location = _view_map_add_location;
  darktable.view_manager->proxy.map.location_action = _view_map_location_action;
  darktable.view_manager->proxy.map.drag_set_icon = _view_map_drag_set_icon;
  darktable.view_manager->proxy.map.redraw = _view_map_redraw;
  darktable.view_manager->proxy.map.display_selected = _view_map_display_selected;

  darktable.view_manager->proxy.map.view = self;

  /* connect signal for filmstrip image activate */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
                            G_CALLBACK(_view_map_filmstrip_activate_callback), self);

  g_timeout_add(250, _view_map_display_selected, self);
}

void leave(dt_view_t *self)
{
  /* disable the map source again. no need to risk network traffic while we are not in map mode. */
  _view_map_set_map_source_g_object(self, OSM_GPS_MAP_SOURCE_NULL);

  /* disconnect from filmstrip image activate */
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_view_map_filmstrip_activate_callback),
                               (gpointer)self);
  g_signal_handlers_disconnect_by_func(dt_ui_thumbtable(darktable.gui->ui)->widget,
                                       G_CALLBACK(_view_map_dnd_remove_callback), self);
  dt_map_t *lib = (dt_map_t *)self->data;
  lib->drop_filmstrip_activated = FALSE;

  if(lib->selected_images)
  {
    g_list_free(lib->selected_images);
    lib->selected_images = NULL;
  }
  gtk_widget_hide(GTK_WIDGET(lib->map));
  gtk_container_remove(GTK_CONTAINER(dt_ui_center_base(darktable.gui->ui)), GTK_WIDGET(lib->map));

  /* reset proxy */
  darktable.view_manager->proxy.map.view = NULL;
}

static void _view_map_undo_callback(dt_action_t *action)
{
  dt_view_t *self = dt_action_view(action);
  dt_map_t *lib = (dt_map_t *)self->data;

  // let current map view unchanged (avoid to center the map on collection)
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_view_map_geotag_changed), self);
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_view_map_collection_changed), self);
  dt_undo_do_undo(darktable.undo, DT_UNDO_MAP);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_view_map_collection_changed), self);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_view_map_geotag_changed), self);
  g_signal_emit_by_name(lib->map, "changed");
}

static void _view_map_redo_callback(dt_action_t *action)
{
  dt_view_t *self = dt_action_view(action);
  dt_map_t *lib = (dt_map_t *)self->data;

  // let current map view unchanged (avoid to center the map on collection)
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_view_map_geotag_changed), self);
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_view_map_collection_changed), self);
  dt_undo_do_redo(darktable.undo, DT_UNDO_MAP);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_view_map_collection_changed), self);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_view_map_geotag_changed), self);
  g_signal_emit_by_name(lib->map, "changed");
}

void gui_init(dt_view_t *self)
{
  dt_action_register(DT_ACTION(self), N_("undo"), _view_map_undo_callback, GDK_KEY_z, GDK_CONTROL_MASK);
  dt_action_register(DT_ACTION(self), N_("redo"), _view_map_redo_callback, GDK_KEY_y, GDK_CONTROL_MASK);
}

static void _view_map_center_on_location(const dt_view_t *view, gdouble lon, gdouble lat, gdouble zoom)
{
  dt_map_t *lib = (dt_map_t *)view->data;
  osm_gps_map_set_center_and_zoom(lib->map, lat, lon, zoom);
}

static void _view_map_center_on_bbox(const dt_view_t *view, gdouble lon1, gdouble lat1, gdouble lon2, gdouble lat2)
{
  dt_map_t *lib = (dt_map_t *)view->data;
  osm_gps_map_zoom_fit_bbox(lib->map, lat1, lat2, lon1, lon2);
}

static void _view_map_show_osd(const dt_view_t *view)
{
  dt_map_t *lib = (dt_map_t *)view->data;
  const gboolean enabled = dt_conf_get_bool("plugins/map/show_map_osd");
  if(enabled)
    osm_gps_map_layer_add(OSM_GPS_MAP(lib->map), lib->osd);
  else
    osm_gps_map_layer_remove(OSM_GPS_MAP(lib->map), lib->osd);

  g_signal_emit_by_name(lib->map, "changed");
}

static void _view_map_set_map_source_g_object(const dt_view_t *view, OsmGpsMapSource_t map_source)
{
  dt_map_t *lib = (dt_map_t *)view->data;

  GValue value = {
    0,
  };
  g_value_init(&value, G_TYPE_INT);
  g_value_set_int(&value, map_source);
  g_object_set_property(G_OBJECT(lib->map), "map-source", &value);
  g_value_unset(&value);
}

static void _view_map_set_map_source(const dt_view_t *view, OsmGpsMapSource_t map_source)
{
  dt_map_t *lib = (dt_map_t *)view->data;

  if(map_source == lib->map_source) return;

  lib->map_source = map_source;
  dt_conf_set_string("plugins/map/map_source", osm_gps_map_source_get_friendly_name(map_source));
  _view_map_set_map_source_g_object(view, map_source);
}

static OsmGpsMapImage *_view_map_add_pin(const dt_view_t *view, GList *points)
{
  dt_map_t *lib = (dt_map_t *)view->data;
  dt_geo_map_display_point_t *p = (dt_geo_map_display_point_t *)points->data;
  return osm_gps_map_image_add_with_alignment(lib->map, p->lat, p->lon, lib->place_pin, 0.5, 1);
}

static gboolean _view_map_remove_pin(const dt_view_t *view, OsmGpsMapImage *pin)
{
  dt_map_t *lib = (dt_map_t *)view->data;
  return osm_gps_map_image_remove(lib->map, pin);
}

static void _track_add_point(OsmGpsMapTrack *track, OsmGpsMapPoint *point, OsmGpsMapPoint *prev_point)
{
  float lat, lon, prev_lat, prev_lon;
  osm_gps_map_point_get_degrees(point, &lat, &lon);
  osm_gps_map_point_get_degrees(prev_point, &prev_lat, &prev_lon);
  double d, delta;
  gboolean short_distance = TRUE;
  if(fabs(lat - prev_lat) > DT_MINIMUM_ANGULAR_DELTA_FOR_GEODESIC
     || fabs(lon - prev_lon) > DT_MINIMUM_ANGULAR_DELTA_FOR_GEODESIC)
  {
    short_distance = FALSE;
    dt_gpx_geodesic_distance(lat, lon,
                             prev_lat, prev_lon,
                             &d, &delta);
  }

  if(short_distance || d < DT_MINIMUM_DISTANCE_FOR_GEODESIC)
  {
    osm_gps_map_track_add_point(track, point);
  }
  else
  {
    /* the line must be splitted in order to seek the geodesic line */
    OsmGpsMapPoint *ith_point;
    double f, ith_lat, ith_lon;
    const int n_segments = ceil(d / DT_MINIMUM_DISTANCE_FOR_GEODESIC);
    gboolean first_time = TRUE;
    for(int i = 1; i <= n_segments; i ++)
    {
      f = (double)i / n_segments;
      dt_gpx_geodesic_intermediate_point(prev_lat, prev_lon,
                                         lat, lon,
                                         delta,
                                         first_time,
                                         f,
                                         &ith_lat, &ith_lon);

      ith_point = osm_gps_map_point_new_degrees (ith_lat, ith_lon);
      osm_gps_map_track_add_point(track, ith_point);
      osm_gps_map_point_free(ith_point);
      first_time = FALSE;
    }
  }
}

#ifdef HAVE_OSMGPSMAP_110_OR_NEWER
static OsmGpsMapPolygon *_view_map_add_polygon(const dt_view_t *view, GList *points)
{
  dt_map_t *lib = (dt_map_t *)view->data;
  OsmGpsMapPolygon *poly = osm_gps_map_polygon_new();
  OsmGpsMapTrack* track = osm_gps_map_track_new();

  // angles for 1 pixel;
  float dlat, dlon;
  _view_map_angles(lib, 1, (lib->bbox.lat1 + lib->bbox.lat2) * 0.5 ,
                           (lib->bbox.lon1 + lib->bbox.lon2) * 0.5 , &dlat, &dlon);

  float prev_lat = 0.0;
  float prev_lon = 0.0;
  for(GList *iter = points; iter; iter = g_list_next(iter))
  {
    dt_geo_map_display_point_t *p = (dt_geo_map_display_point_t *)iter->data;
    if((fabs(p->lat - prev_lat) > dlat) || (fabs(p->lon - prev_lon) > dlon))
    {
      OsmGpsMapPoint* point = osm_gps_map_point_new_degrees(p->lat, p->lon);
      osm_gps_map_track_add_point(track, point);
      prev_lat = p->lat;
      prev_lon = p->lon;
    }
  }

  g_object_set(poly, "track", track, (gchar *)0);
  g_object_set(poly, "editable", FALSE, (gchar *)0);
  g_object_set(poly, "shaded", FALSE, (gchar *)0);

  osm_gps_map_polygon_add(lib->map, poly);

  return poly;
}

static OsmGpsMapPolygon *_view_map_add_polygon_location(dt_map_t *lib, dt_location_draw_t *ld)
{
  OsmGpsMapPolygon *poly = osm_gps_map_polygon_new();
  OsmGpsMapTrack* track = osm_gps_map_track_new();
  g_object_set(track, "line-width" , 2.0, "alpha", 0.9, (gchar *)0);

  // angles for 1 pixel;
  float dlat, dlon;
  _view_map_angles(lib, 1, ld->data.lat, ld->data.lon, &dlat, &dlon);
  int zoom;
  g_object_get(G_OBJECT(lib->map), "zoom", &zoom, NULL);
  // zoom = 20 => mod = 21 ; zoom = 0 => mod = 1
  const int mod2 = zoom + 1;

  // keep a bit of points around the bounding box
  dt_map_box_t bbox;
  _view_map_get_bounding_box(lib, &bbox);
  const float mlon = (bbox.lon2 - bbox.lon1) * 0.5;
  const float mlat = (bbox.lat1 - bbox.lat2) * 0.5;
  bbox.lon1 = CLAMP(bbox.lon1 - mlon, -180.0, 180);
  bbox.lon2 = CLAMP(bbox.lon2 + mlon, -180.0, 180);
  bbox.lat1 = CLAMP(bbox.lat1 + mlat, -90.0, 90);
  bbox.lat2 = CLAMP(bbox.lat2 - mlat, -90.0, 90);

  int i = 0;
  float prev_lat = 0.0;
  float prev_lon = 0.0;
  for(GList *iter = ld->data.polygons; iter; iter = g_list_next(iter), i++)
  {
    dt_geo_map_display_point_t *p = (dt_geo_map_display_point_t *)iter->data;
    if(p->lat <= bbox.lat1 && p->lat >= bbox.lat2 &&
       p->lon >= bbox.lon1 && p->lon <= bbox.lon2)
    {
      if((fabs(p->lat - prev_lat) > dlat) || (fabs(p->lon - prev_lon) > dlon))
      {
        OsmGpsMapPoint* point = osm_gps_map_point_new_degrees(p->lat, p->lon);
        osm_gps_map_track_add_point(track, point);
        prev_lat = p->lat;
        prev_lon = p->lon;
      }
    }
    else if(!(i % mod2))
    {
      OsmGpsMapPoint* point = osm_gps_map_point_new_degrees(p->lat, p->lon);
      osm_gps_map_track_add_point(track, point);
    }
  }

  g_object_set(poly, "track", track, (gchar *)0);
  g_object_set(poly, "editable", FALSE, (gchar *)0);
  g_object_set(poly, "shaded", FALSE, (gchar *)0);

  osm_gps_map_polygon_add(lib->map, poly);

  return poly;
}

static gboolean _view_map_remove_polygon(const dt_view_t *view, OsmGpsMapPolygon *polygon)
{
  dt_map_t *lib = (dt_map_t *)view->data;
  return osm_gps_map_polygon_remove(lib->map, polygon);
}
#endif

static OsmGpsMapTrack *_view_map_add_track(const dt_view_t *view, GList *points)
{
  dt_map_t *lib = (dt_map_t *)view->data;

  OsmGpsMapTrack* track = osm_gps_map_track_new();

  OsmGpsMapPoint* prev_point;
  gboolean first_point = TRUE;
  for(GList *iter = points; iter; iter = g_list_next(iter))
  {
    dt_geo_map_display_point_t *p = (dt_geo_map_display_point_t *)iter->data;
    OsmGpsMapPoint* point = osm_gps_map_point_new_degrees(p->lat, p->lon);
    if(first_point)
    {
      osm_gps_map_track_add_point(track, point);
    }
    else
    {
      _track_add_point(track, point, prev_point);
      osm_gps_map_point_free(prev_point);
    }
    prev_point = &(*point);
    if(!g_list_next(iter))
      osm_gps_map_point_free(prev_point);
    first_point = FALSE;
  }

  g_object_set(track, "editable", FALSE, (gchar *)0);

  osm_gps_map_track_add(lib->map, track);

  return track;
}

static gboolean _view_map_remove_track(const dt_view_t *view, OsmGpsMapTrack *track)
{
  dt_map_t *lib = (dt_map_t *)view->data;
  return osm_gps_map_track_remove(lib->map, track);
}

static OsmGpsMapImage *_view_map_draw_single_image(const dt_view_t *view, GList *points)
{
  dt_map_t *lib = (dt_map_t *)view->data;
  struct {dt_imgid_t imgid; float latitude; float longitude; int count;} *p;
  p = points->data;
  GdkPixbuf *thumb = _draw_image(p->imgid, NULL, NULL, p->count, TRUE,
                                 thumb_frame_gpx_color, TRUE, DT_MAP_THUMB_THUMB, (dt_view_t *)view);
  OsmGpsMapImage *image = NULL;
  if(thumb)
  {
    image = osm_gps_map_image_add_with_alignment(lib->map, p->latitude, p->longitude, thumb, 0, 1);
    g_object_unref(thumb);
  }
  return image;
}

static GObject *_view_map_add_marker(const dt_view_t *view, dt_geo_map_display_t type, GList *points)
{
  switch(type)
  {
    case MAP_DISPLAY_POINT: return G_OBJECT(_view_map_add_pin(view, points));
    case MAP_DISPLAY_TRACK: return G_OBJECT(_view_map_add_track(view, points));
#ifdef HAVE_OSMGPSMAP_110_OR_NEWER
    case MAP_DISPLAY_POLYGON: return G_OBJECT(_view_map_add_polygon(view, points));
#endif
    case MAP_DISPLAY_THUMB: return G_OBJECT(_view_map_draw_single_image(view, points));
    default: return NULL;
  }
}

static gboolean _view_map_remove_marker(const dt_view_t *view, dt_geo_map_display_t type, GObject *marker)
{
  dt_map_t *lib = (dt_map_t *)view->data;
  if(type == MAP_DISPLAY_NONE) return FALSE;

  switch(type)
  {
    case MAP_DISPLAY_POINT: return _view_map_remove_pin(view, OSM_GPS_MAP_IMAGE(marker));
    case MAP_DISPLAY_TRACK: return _view_map_remove_track(view, OSM_GPS_MAP_TRACK(marker));
#ifdef HAVE_OSMGPSMAP_110_OR_NEWER
    case MAP_DISPLAY_POLYGON: return _view_map_remove_polygon(view, OSM_GPS_MAP_POLYGON(marker));
#endif
    case MAP_DISPLAY_THUMB: return osm_gps_map_image_remove(lib->map, OSM_GPS_MAP_IMAGE(marker));
    default: return FALSE;
  }
}

static void _view_map_add_location(const dt_view_t *view, dt_map_location_data_t *g, const guint locid)
{
  dt_map_t *lib = (dt_map_t *)view->data;
  dt_location_draw_t loc_main;
  loc_main.id = locid;
  if(g)
  {
    if(g->delta1 != 0.0 && g->delta2 != 0.0)
    {
      // existing location
      memcpy(&loc_main.data, g, sizeof(dt_map_location_data_t));
      const double max_lon = CLAMP(g->lon + g->delta1, -180, 180);
      const double min_lon = CLAMP(g->lon - g->delta1, -180, 180);
      const double max_lat = CLAMP(g->lat + g->delta2, -90, 90);
      const double min_lat = CLAMP(g->lat - g->delta2, -90, 90);
      if(max_lon > min_lon && max_lat > min_lat)
      {
        // only if new box not imcluded in the current map box
        if(g->lon < lib->bbox.lon1 || g->lon > lib->bbox.lon2 ||
           g->lat > lib->bbox.lat1 || g->lat < lib->bbox.lat2)
          _view_map_center_on_bbox(view, min_lon, max_lat, max_lon, min_lat);
        _view_map_draw_main_location(lib, &loc_main);
      }
    }
    else
    {
      // this is a new location
      loc_main.data.shape = g->shape;
      if(g->shape == MAP_LOCATION_SHAPE_POLYGONS)
      {
        dt_map_box_t bbox;
        loc_main.data.polygons = dt_map_location_convert_polygons(g->polygons, &bbox, &loc_main.data.plg_pts);
        _view_map_center_on_bbox(view, bbox.lon1, bbox.lat2, bbox.lon2, bbox.lat1);
        loc_main.data.lon = (bbox.lon1 + bbox.lon2) * 0.5;
        loc_main.data.lat = (bbox.lat1 + bbox.lat2) * 0.5;
        loc_main.data.ratio = 1;
        loc_main.data.delta1 = (bbox.lon2 - bbox.lon1) * 0.5;
        loc_main.data.delta2 = (bbox.lat1 - bbox.lat2) * 0.5;
      }
      else
      {
        // create the location on the center of the map
        float lon, lat;
        g_object_get(G_OBJECT(lib->map), "latitude", &lat, "longitude", &lon, NULL);
        loc_main.data.lon = lon, loc_main.data.lat = lat;
        // get a radius angle equivalent to thumb dimension to start with for delta1
        float dlat, dlon;
        _view_map_thumb_angles(lib, loc_main.data.lat, loc_main.data.lon, &dlat, &dlon);
        loc_main.data.ratio = _view_map_get_angles_ratio(lib, loc_main.data.lat, loc_main.data.lon);
        loc_main.data.delta1 = dlon;
        loc_main.data.delta2 = dlon / loc_main.data.ratio;
      }
      _view_map_draw_main_location(lib, &loc_main);
      _view_map_update_location_geotag((dt_view_t *)view);
    }
  }
}

static void _view_map_location_action(const dt_view_t *view, const int action)
{
  dt_map_t *lib = (dt_map_t *)view->data;
  if(action == MAP_LOCATION_ACTION_REMOVE && lib->loc.main.id)
  {
    GList *other = _others_location(lib->loc.others, lib->loc.main.id);
    if(other)
      _view_map_delete_other_location(lib, other);
    // remove the main location
    _view_map_remove_location(lib, &lib->loc.main);
    lib->loc.main.id = 0;
  }
  _view_map_draw_other_locations(lib, &lib->bbox);
}


static void _view_map_check_preference_changed(gpointer instance, gpointer user_data)
{
  dt_view_t *view = (dt_view_t *)user_data;
  dt_map_t *lib = (dt_map_t *)view->data;

  if(_view_map_prefs_changed(lib)) g_signal_emit_by_name(lib->map, "changed");
}

static void _view_map_collection_changed(gpointer instance, dt_collection_change_t query_change,
                                         dt_collection_properties_t changed_property, gpointer imgs, int next,
                                         gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_map_t *lib = (dt_map_t *)self->data;
  // avoid to centre the map on collection while a location is active
  if(darktable.view_manager->proxy.map.view && !lib->loc.main.id)
  {
    _view_map_center_on_image_list(self, "memory.collected_images");
  }

  if(dt_conf_get_bool("plugins/map/filter_images_drawn"))
  {
    // only redraw when map mode is currently active, otherwise enter() does the magic
    if(darktable.view_manager->proxy.map.view) g_signal_emit_by_name(lib->map, "changed");
  }
}

static void _view_map_selection_changed(gpointer instance, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_map_t *lib = (dt_map_t *)self->data;

  /* only redraw when map mode is currently active, otherwise enter() does the magic */
  if(darktable.view_manager->proxy.map.view) g_signal_emit_by_name(lib->map, "changed");
}

static void _view_map_geotag_changed(gpointer instance, GList *imgs, const int locid, gpointer user_data)
{
  // if locid <> NULL this event doesn't concern geotag but location
  if(!locid)
  {
    dt_view_t *self = (dt_view_t *)user_data;
    dt_map_t *lib = (dt_map_t *)self->data;
    if(darktable.view_manager->proxy.map.view) g_signal_emit_by_name(lib->map, "changed");
  }
}

static void _view_map_center_on_image(dt_view_t *self, const dt_imgid_t imgid)
{
  if(imgid)
  {
    const dt_map_t *lib = (dt_map_t *)self->data;
    dt_image_geoloc_t geoloc;
    dt_image_get_location(imgid, &geoloc);

    if(geoloc.longitude != DT_INVALID_GPS_COORDINATE && geoloc.latitude != DT_INVALID_GPS_COORDINATE)
    {
      int zoom;
      g_object_get(G_OBJECT(lib->map), "zoom", &zoom, NULL);
      _view_map_center_on_location(self, geoloc.longitude, geoloc.latitude, zoom);
    }
  }
}

static gboolean _view_map_center_on_image_list(dt_view_t *self, const char* table)
{
  const dt_map_t *lib = (dt_map_t *)self->data;
  double max_longitude = -FLT_MAX;
  double max_latitude = -FLT_MAX;
  double min_longitude = FLT_MAX;
  double min_latitude = FLT_MAX;
  int count = 0;

  // clang-format off
  gchar *query = g_strdup_printf("SELECT MIN(latitude), MAX(latitude),"
                                "       MIN(longitude), MAX(longitude), COUNT(*)"
                                " FROM main.images AS i "
                                " JOIN %s AS l ON l.imgid = i.id "
                                " WHERE latitude NOT NULL AND longitude NOT NULL",
                                table);
  // clang-format on
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    min_latitude = sqlite3_column_double(stmt, 0);
    max_latitude = sqlite3_column_double(stmt, 1);
    min_longitude = sqlite3_column_double(stmt, 2);
    max_longitude  = sqlite3_column_double(stmt, 3);
    count = sqlite3_column_int(stmt, 4);
  }
  sqlite3_finalize(stmt);
  g_free(query);

  if(count>0)
  {
    max_longitude = CLAMP(max_longitude, -180, 180);
    min_longitude = CLAMP(min_longitude, -180, 180);
    max_latitude = CLAMP(max_latitude, -90, 90);
    min_latitude = CLAMP(min_latitude, -90, 90);

    _view_map_center_on_bbox(self, min_longitude, min_latitude, max_longitude, max_latitude);

    // Now the zoom is set we can use the thumb angle to give some room
    max_longitude = CLAMP(max_longitude + 1.0 * lib->thumb_lon_angle, -180, 180);
    min_longitude = CLAMP(min_longitude - 0.2 * lib->thumb_lon_angle, -180, 180);
    max_latitude = CLAMP(max_latitude + 1.0 * lib->thumb_lat_angle, -90, 90);
    min_latitude = CLAMP(min_latitude - 0.2 * lib->thumb_lat_angle, -90, 90);

    _view_map_center_on_bbox(self, min_longitude, min_latitude, max_longitude, max_latitude);

    return TRUE;
  }
  else
    return FALSE;
}

static void _view_map_filmstrip_activate_callback(gpointer instance, dt_imgid_t imgid, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  _view_map_center_on_image(self, imgid);
}

static void _drag_and_drop_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                   GtkSelectionData *selection_data, guint target_type, guint time,
                                   gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_map_t *lib = (dt_map_t *)self->data;
  gboolean success = FALSE;
  if(selection_data != NULL && target_type == DND_TARGET_IMGID)
  {
    const int imgs_nb = gtk_selection_data_get_length(selection_data) / sizeof(uint32_t);
    if(imgs_nb)
    {
      uint32_t *imgt = (uint32_t *)gtk_selection_data_get_data(selection_data);
      if(imgs_nb == 1 && imgt[0] == -1)
      {
        // move of location
        OsmGpsMapPoint *pt = osm_gps_map_point_new_degrees(0.0, 0.0);
        osm_gps_map_convert_screen_to_geographic(lib->map, x, y, pt);
        float lat, lon;
        osm_gps_map_point_get_degrees(pt, &lat, &lon);
        lib->loc.main.data.lat = lat, lib->loc.main.data.lon = lon;
        const float prev_ratio = lib->loc.main.data.ratio;
        lib->loc.main.data.ratio = _view_map_get_angles_ratio(lib, lib->loc.main.data.lat,
                                   lib->loc.main.data.lon);
        lib->loc.main.data.delta2 = lib->loc.main.data.delta2 * prev_ratio / lib->loc.main.data.ratio;
        osm_gps_map_point_free(pt);
        _view_map_update_location_geotag(self);
        _view_map_draw_main_location(lib, &lib->loc.main);
        _view_map_signal_change_wait(self, 1);
        success = TRUE;
      }
      else
      {
        GList *imgs = NULL;
        for(int i = 0; i < imgs_nb; i++)
        {
          imgs = g_list_prepend(imgs, GINT_TO_POINTER(imgt[i]));
        }
        float longitude, latitude;
        OsmGpsMapPoint *pt = osm_gps_map_point_new_degrees(0.0, 0.0);
        osm_gps_map_convert_screen_to_geographic(lib->map, x - lib->start_drag_offset_x,
                                                 y - lib->start_drag_offset_y, pt);
        osm_gps_map_point_get_degrees(pt, &latitude, &longitude);
        osm_gps_map_point_free(pt);
        // TODO redraw the image group
        // it seems that at this time osm_gps_map doesn't answer before dt_image_set_locations(). Locked in some way ?
        const dt_image_geoloc_t geoloc = { longitude, latitude, DT_INVALID_GPS_COORDINATE };
        dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_view_map_collection_changed), self);
        dt_image_set_locations(imgs, &geoloc, TRUE);
        dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_view_map_collection_changed), self);
        DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_GEOTAG_CHANGED, imgs, 0);
        g_signal_emit_by_name(lib->map, "changed");
        success = TRUE;
      }
    }
  }
  gtk_drag_finish(context, success, FALSE, time);
}

static void _view_map_dnd_get_callback(GtkWidget *widget, GdkDragContext *context,
                                       GtkSelectionData *selection_data, guint target_type, guint time,
                                       dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  g_assert(selection_data != NULL);
  switch(target_type)
  {
    case DND_TARGET_IMGID:
      {
        if(lib->selected_images)
        {
          // drag & drop of images
          const guint imgs_nb = g_list_length(lib->selected_images);
          if(imgs_nb)
          {
            uint32_t *imgs = malloc(sizeof(uint32_t) * imgs_nb);
            int i = 0;
            for(GList *l = lib->selected_images; l; l = g_list_next(l))
            {
              imgs[i++] = GPOINTER_TO_INT(l->data);
            }
            gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data),
                                   _DWORD, (guchar *)imgs, imgs_nb * sizeof(uint32_t));
            free(imgs);
          }
        }
        else if(lib->loc.main.id > 0)
        {
          // move of location
          uint32_t *imgs = malloc(sizeof(uint32_t));
          imgs[0] = -1;
          gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data),
                                 _DWORD, (guchar *)imgs, sizeof(uint32_t));
          free(imgs);
        }
      }
      break;
    default: // return the location of the file as a last resort
    case DND_TARGET_URI:
    {
      if(lib->selected_images)
      {
        const dt_imgid_t imgid = GPOINTER_TO_INT(lib->selected_images->data);
        gchar pathname[PATH_MAX] = { 0 };
        gboolean from_cache = TRUE;
        dt_image_full_path(imgid, pathname, sizeof(pathname), &from_cache);
        gchar *uri = g_strdup_printf("file://%s", pathname); // TODO: should we add the host?
        gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data), _BYTE,
                               (guchar *)uri, strlen(uri));
        g_free(uri);
      }
      break;
    }
  }
}

static void _view_map_dnd_remove_callback(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                          GtkSelectionData *selection_data, guint target_type, guint time,
                                          gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_map_t *lib = (dt_map_t *)self->data;

  gboolean success = FALSE;

  if(selection_data != NULL && target_type == DND_TARGET_IMGID)
  {
    const int imgs_nb = gtk_selection_data_get_length(selection_data) / sizeof(uint32_t);
    if(imgs_nb)
    {
      uint32_t *imgt = (uint32_t *)gtk_selection_data_get_data(selection_data);
      GList *imgs = NULL;
      for(int i = 0; i < imgs_nb; i++)
      {
        imgs = g_list_prepend(imgs, GINT_TO_POINTER(imgt[i]));
      }
      //  image(s) dropped into the filmstrip, let's remove it (them) in this case
      const dt_image_geoloc_t geoloc
        = { DT_INVALID_GPS_COORDINATE, DT_INVALID_GPS_COORDINATE, DT_INVALID_GPS_COORDINATE };
      dt_image_set_locations(imgs, &geoloc, TRUE);
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_GEOTAG_CHANGED, imgs, 0);
      success = TRUE;
    }
  }
  gtk_drag_finish(context, success, FALSE, time);
  if(success) g_signal_emit_by_name(lib->map, "changed");
}

static gboolean _view_map_dnd_failed_callback(GtkWidget *widget, GdkDragContext *drag_context,
                                              GtkDragResult result, dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  g_signal_emit_by_name(lib->map, "changed");

  return TRUE;
}

static gboolean _view_map_prefs_changed(dt_map_t *lib)
{
  gboolean prefs_changed = FALSE;

  lib->max_images_drawn = dt_conf_get_int("plugins/map/max_images_drawn");
  if(lib->max_images_drawn == 0) lib->max_images_drawn = 100;

  gboolean filter_images_drawn = dt_conf_get_bool("plugins/map/filter_images_drawn");
  if(lib->filter_images_drawn != filter_images_drawn) prefs_changed = TRUE;

  const char *thumbnail = dt_conf_get_string_const("plugins/map/images_thumbnail");
  lib->thumbnail = !g_strcmp0(thumbnail, "thumbnail") ? DT_MAP_THUMB_THUMB :
                   !g_strcmp0(thumbnail, "count") ? DT_MAP_THUMB_COUNT : DT_MAP_THUMB_NONE;

  return prefs_changed;
}

static void _view_map_build_main_query(dt_map_t *lib)
{
  char *geo_query;

  if(lib->main_query) sqlite3_finalize(lib->main_query);

  lib->filter_images_drawn = dt_conf_get_bool("plugins/map/filter_images_drawn");
  // clang-format off
  geo_query = g_strdup_printf("SELECT * FROM"
                              " (SELECT id, longitude, latitude "
                              "   FROM %s WHERE longitude >= ?1 AND longitude <= ?2"
                              "           AND latitude <= ?3 AND latitude >= ?4 "
                              "           AND longitude NOT NULL AND latitude NOT NULL)"
                              "   ORDER BY longitude ASC",  // critical to make dbscan work
                              lib->filter_images_drawn
                              ? "main.images i INNER JOIN memory.collected_images c ON i.id = c.imgid"
                              : "main.images");
  // clang-format on

  /* prepare the main query statement */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), geo_query, -1, &lib->main_query, NULL);

  g_free(geo_query);
}

GSList *mouse_actions(const dt_view_t *self)
{
  GSList *lm = NULL;
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_DOUBLE_LEFT, 0, _("[on image] open in darkroom"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_DOUBLE_LEFT, 0, _("[on map] zoom map"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_DRAG_DROP, 0, _("move image location"));
  return lm;
}

// starting point taken from https://github.com/gyaikhom/dbscan
// Copyright 2015 Gagarine Yaikhom (MIT License)

typedef struct epsilon_neighbours_t
{
  unsigned int num_members;
  unsigned int index[];
} epsilon_neighbours_t;

typedef struct dt_dbscan_t
{
  dt_geo_position_t *points;
  unsigned int num_points;
  double epsilon;
  unsigned int minpts;
  epsilon_neighbours_t *seeds;
  epsilon_neighbours_t *spreads;
  unsigned int index;
  unsigned int cluster_id;
} dt_dbscan_t;

static dt_dbscan_t db;

static void _get_epsilon_neighbours(epsilon_neighbours_t *en, unsigned int index)
{
  // points are ordered by longitude
  // limit the exploration to epsilon east and west
  // west
  for(int i = index; i < db.num_points; ++i)
  {
    if(i == index || db.points[i].cluster_id >= 0)
      continue;
    if((db.points[i].x - db.points[index].x) > db.epsilon)
      break;
    if(fabs(db.points[i].y - db.points[index].y) > db.epsilon)
      continue;
    else
    {
      en->index[en->num_members] = i;
      en->num_members++;
    }
  }
  // east
  for(int i = index; i >= 0; --i)
  {
    if(i == (int)index || db.points[i].cluster_id >= 0)
      continue;
    if((db.points[index].x - db.points[i].x) > db.epsilon)
      break;
    if(fabs(db.points[index].y - db.points[i].y) > db.epsilon)
      continue;
    else
    {
      en->index[en->num_members] = i;
      en->num_members++;
    }
  }
}

static void _dbscan_spread(unsigned int index)
{
  db.spreads->num_members = 0;
  _get_epsilon_neighbours(db.spreads, index);

  for(unsigned int i = 0; i < db.spreads->num_members; i++)
  {
    dt_geo_position_t *d = &db.points[db.spreads->index[i]];
    if(d->cluster_id == NOISE || d->cluster_id == UNCLASSIFIED)
    {
      db.seeds->index[db.seeds->num_members] = db.spreads->index[i];
      db.seeds->num_members++;
      d->cluster_id = db.cluster_id;
    }
  }
}

static int _dbscan_expand(unsigned int index)
{
  int return_value = NOT_CORE_POINT;
  db.seeds->num_members = 0;
  _get_epsilon_neighbours(db.seeds, index);

  if(db.seeds->num_members < db.minpts)
    db.points[index].cluster_id = NOISE;
  else
  {
    db.points[index].cluster_id = db.cluster_id;
    for(int i = 0; i < db.seeds->num_members; i++)
    {
      db.points[db.seeds->index[i]].cluster_id = db.cluster_id;
    }

    for(int i = 0; i < db.seeds->num_members; i++)
    {
      _dbscan_spread(db.seeds->index[i]);
    }
    return_value = CORE_POINT;
  }
  return return_value;
}

static void _dbscan(dt_geo_position_t *points, unsigned int num_points,
                    double epsilon, unsigned int minpts)
{
  db.points = points;
  db.num_points = num_points;
  db.epsilon = epsilon;
  // remove the pivot from target
  db.minpts = minpts > 1 ? minpts - 1 : minpts;
  db.cluster_id = 0;
  db.seeds = (epsilon_neighbours_t *)malloc(sizeof(db.seeds->num_members)
      + num_points * sizeof(db.seeds->index[0]));
  db.spreads = (epsilon_neighbours_t *)malloc(sizeof(db.spreads->num_members)
      + num_points * sizeof(db.spreads->index[0]));

  if(db.seeds && db.spreads)
  {
    for(unsigned int i = 0; i < db.num_points; ++i)
    {
      if(db.points[i].cluster_id == UNCLASSIFIED)
      {
        if(_dbscan_expand(i) == CORE_POINT)
        {
          ++db.cluster_id;
        }
      }
    }
  g_free(db.seeds);
  g_free(db.spreads);
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

