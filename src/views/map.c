/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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

typedef struct dt_map_t
{
  gboolean entering;
  OsmGpsMap *map;
  OsmGpsMapSource_t map_source;
  OsmGpsMapLayer *osd;
  GSList *images;
  GdkPixbuf *image_pin, *place_pin;
  GList *selected_images;
  gboolean start_drag;
  float thumb_lat_angle, thumb_lon_angle;
  sqlite3_stmt *main_query;
  gboolean drop_filmstrip_activated;
  gboolean filter_images_drawn;
  int max_images_drawn;
  struct
  {
    OsmGpsMapImage *location;
    guint id;
    dt_map_location_data_t data;
    gboolean drag;
    int time_out;
  } loc;
} dt_map_t;

typedef struct dt_map_image_t
{
  gint imgid;
  double latitude;
  double longitude;
  double cum_lat;
  double cum_lon;
  int group;
  int group_count;
  gboolean group_same_loc;
  gboolean selected_in_group;
  OsmGpsMapImage *image;
  gint width, height;
} dt_map_image_t;

static const int thumb_size = 128, thumb_border = 2, image_pin_size = 13, place_pin_size = 72;
static const int cross_size = 16, max_size = 1024;
static const float thumb_overlap = 1.2f;
static const uint32_t thumb_frame_color = 0x000000aa;
static const uint32_t thumb_frame_sel_color = 0xffffffee;
static const uint32_t pin_outer_color = 0x0000aaaa;
static const uint32_t pin_inner_color = 0xffffffee;
static const uint32_t pin_line_color = 0x000000ff;

/* proxy function to center map view on location at a zoom level */
static void _view_map_center_on_location(const dt_view_t *view, gdouble lon, gdouble lat, gdouble zoom);
/* proxy function to center map view on a bounding box */
static void _view_map_center_on_bbox(const dt_view_t *view, gdouble lon1, gdouble lat1, gdouble lon2, gdouble lat2);
/* proxy function to show or hide the osd */
static void _view_map_show_osd(const dt_view_t *view, gboolean enabled);
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
static void _view_map_remove_location(const dt_view_t *view);

/* callback when the collection changes */
static void _view_map_collection_changed(gpointer instance, dt_collection_change_t query_change, gpointer imgs,
                                         int next, gpointer user_data);
/* callback when the selection changes */
static void _view_map_selection_changed(gpointer instance, gpointer user_data);
/* update the geotag information on location tag */
static void _view_map_update_location_geotag(dt_view_t *self);
/* callback when an image is selected in filmstrip, centers map */
static void _view_map_filmstrip_activate_callback(gpointer instance, int imgid, gpointer user_data);
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
/* callback that handles double clicks on the map */
static gboolean _view_map_button_press_callback(GtkWidget *w, GdkEventButton *e, dt_view_t *self);
/* callback when the mouse is moved */
static gboolean _view_map_motion_notify_callback(GtkWidget *w, GdkEventMotion *e, dt_view_t *self);
static gboolean _view_map_dnd_failed_callback(GtkWidget *widget, GdkDragContext *drag_context,
                                              GtkDragResult result, dt_view_t *self);
static void _view_map_dnd_remove_callback(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                          GtkSelectionData *selection_data, guint target_type, guint time,
                                          gpointer data);

static gboolean _view_map_prefs_changed(dt_map_t *lib);
static void _view_map_build_main_query(dt_map_t *lib);

/* center map to on the baricenter of the image list */
static gboolean _view_map_center_on_image_list(dt_view_t *self, const char *table);
/* center map on the given image */
static void _view_map_center_on_image(dt_view_t *self, const int32_t imgid);

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
    int zoom = luaL_checkinteger(L, 3);
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
  float lat1_m = atanh(sin(lat1));
  float lat2_m = atanh(sin(lat2));
  int zoom_lon = LOG2((double)(2 * pix_width * M_PI) / (TILESIZE * (lon2 - lon1)));
  int zoom_lat = LOG2((double)(2 * pix_height * M_PI) / (TILESIZE * (lat2_m - lat1_m)));
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
  int zoom;
  gtk_widget_get_allocation(GTK_WIDGET (map), &allocation);
  zoom = latlon2zoom(allocation.height, allocation.width, deg2rad(latitude1), deg2rad(latitude2), deg2rad(longitude1), deg2rad(longitude2));
  osm_gps_map_set_center(map, (latitude1 + latitude2) / 2, (longitude1 + longitude2) / 2);
  osm_gps_map_set_zoom(map, zoom);
}
#endif // HAVE_OSMGPSMAP_110_OR_NEWER

static GdkPixbuf *_view_map_images_count(const int nb_images, const gboolean same_loc,
                                         double *count_width, double *count_height)
{
  char text[8] = {0};
  snprintf(text, sizeof(text), "%d", nb_images > 99999 ? 99999 : nb_images);

  int w = DT_PIXEL_APPLY_DPI(thumb_size + 2 * thumb_border), h = DT_PIXEL_APPLY_DPI(image_pin_size);

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
  size_t size = w * h * 4;
  uint8_t *buf = (uint8_t *)malloc(size);
  memcpy(buf, data, size);
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(buf, GDK_COLORSPACE_RGB, TRUE, 8, w, h, w * 4,
                                               (GdkPixbufDestroyNotify)free, NULL);
  cairo_surface_destroy(cst);
  return pixbuf;
}

static GdkPixbuf *_init_image_pin()
{
  int w = DT_PIXEL_APPLY_DPI(thumb_size + 2 * thumb_border), h = DT_PIXEL_APPLY_DPI(image_pin_size);
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
  size_t size = w * h * 4;
  uint8_t *buf = (uint8_t *)malloc(size);
  memcpy(buf, data, size);
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(buf, GDK_COLORSPACE_RGB, TRUE, 8, w, h, w * 4,
                                               (GdkPixbufDestroyNotify)free, NULL);
  cairo_surface_destroy(cst);
  return pixbuf;
}

static GdkPixbuf *_init_place_pin()
{
  int w = DT_PIXEL_APPLY_DPI(place_pin_size), h = DT_PIXEL_APPLY_DPI(place_pin_size);
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
  size_t size = w * h * 4;
  uint8_t *buf = (uint8_t *)malloc(size);
  memcpy(buf, data, size);
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(buf, GDK_COLORSPACE_RGB, TRUE, 8, w, h, w * 4,
                                               (GdkPixbufDestroyNotify)free, NULL);
  cairo_surface_destroy(cst);
  return pixbuf;
}

static GdkPixbuf *_draw_circle(const float radius)
{
  const int rad = radius > max_size ? max_size :
                  radius < cross_size ? cross_size : radius;
  const int w = DT_PIXEL_APPLY_DPI(2.0 * rad);
  const int h = w;
  const int d = DT_PIXEL_APPLY_DPI(1);
  const int cross = DT_PIXEL_APPLY_DPI(cross_size);
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(cst);

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1));
  dt_gui_gtk_set_source_rgb(cr, rad == max_size || rad == cross_size
                                ? DT_GUI_COLOR_MAP_LOC_SHAPE_DEF
                                : DT_GUI_COLOR_MAP_LOC_SHAPE_HIGH);
  cairo_arc(cr, 0.5 * w, 0.5 * h, 0.5 * h - d, 0, 2.0 * M_PI);
  cairo_move_to(cr, 0.5 * w, 0.5 * h - cross);
  cairo_line_to(cr, 0.5 * w, 0.5 * h + cross);
  cairo_move_to(cr, 0.5 * w - cross, 0.5 * h );
  cairo_line_to(cr, 0.5 * w + cross, 0.5 * h );
  cairo_stroke(cr);
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_MAP_LOC_SHAPE_LOW);
  cairo_arc(cr, 0.5 * w, 0.5 * h, 0.5 * h - d - d, 0, 2.0 * M_PI);
  cairo_move_to(cr, 0.5 * w + d, 0.5 * h - cross);
  cairo_line_to(cr, 0.5 * w + d, 0.5 * h + cross);
  cairo_move_to(cr, 0.5 * w - cross, 0.5 * h - d);
  cairo_line_to(cr, 0.5 * w + cross, 0.5 * h - d);
  cairo_stroke(cr);

  cairo_destroy(cr);
  uint8_t *data = cairo_image_surface_get_data(cst);
  dt_draw_cairo_to_gdk_pixbuf(data, w, h);
  size_t size = w * h * 4;
  uint8_t *buf = (uint8_t *)malloc(size);
  memcpy(buf, data, size);
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(buf, GDK_COLORSPACE_RGB, TRUE, 8, w, h, w * 4,
                                               (GdkPixbufDestroyNotify)free, NULL);
  cairo_surface_destroy(cst);
  return pixbuf;
}

static GdkPixbuf *_draw_rectangle(const float dlongitude, const float dlatitude)
{
  const int dlon = dlongitude > max_size ? max_size :
                  dlongitude < cross_size ? cross_size : dlongitude;
  const int dlat = dlatitude > max_size ? max_size :
                  dlatitude < cross_size ? cross_size : dlatitude;
  const int w = DT_PIXEL_APPLY_DPI(2.0 * dlon);
  const int h = DT_PIXEL_APPLY_DPI(2.0 * dlat);
  const int d = DT_PIXEL_APPLY_DPI(1);
  const int cross = DT_PIXEL_APPLY_DPI(cross_size);
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(cst);

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1));
  dt_gui_gtk_set_source_rgb(cr, dlon == max_size || dlon == cross_size ||
                                dlat == max_size || dlat == cross_size
                                ? DT_GUI_COLOR_MAP_LOC_SHAPE_DEF
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

  cairo_destroy(cr);
  uint8_t *data = cairo_image_surface_get_data(cst);
  dt_draw_cairo_to_gdk_pixbuf(data, w, h);
  size_t size = w * h * 4;
  uint8_t *buf = (uint8_t *)malloc(size);
  memcpy(buf, data, size);
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(buf, GDK_COLORSPACE_RGB, TRUE, 8, w, h, w * 4,
                                               (GdkPixbufDestroyNotify)free, NULL);
  cairo_surface_destroy(cst);
  return pixbuf;
}

static GdkPixbuf *_draw_cross()
{
  const int cross = DT_PIXEL_APPLY_DPI(cross_size);
  const int w = 2.0 * cross;
  const int h = w;
  const int d = DT_PIXEL_APPLY_DPI(1);
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(cst);

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1));
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_MAP_LOC_SHAPE_HIGH);
  cairo_move_to(cr, 0.5 * w, 0.5 * h - cross);
  cairo_line_to(cr, 0.5 * w, 0.5 * h + cross);
  cairo_move_to(cr, 0.5 * w - cross, 0.5 * h );
  cairo_line_to(cr, 0.5 * w + cross, 0.5 * h );
  cairo_stroke(cr);
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_MAP_LOC_SHAPE_LOW);
  cairo_move_to(cr, 0.5 * w + d, 0.5 * h - cross);
  cairo_line_to(cr, 0.5 * w + d, 0.5 * h + cross);
  cairo_move_to(cr, 0.5 * w - cross, 0.5 * h - d);
  cairo_line_to(cr, 0.5 * w + cross, 0.5 * h - d);
  cairo_stroke(cr);

  cairo_destroy(cr);
  uint8_t *data = cairo_image_surface_get_data(cst);
  dt_draw_cairo_to_gdk_pixbuf(data, w, h);
  size_t size = w * h * 4;
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
    _view_map_remove_location(self);
  }
}

void init(dt_view_t *self)
{
  self->data = calloc(1, sizeof(dt_map_t));

  dt_map_t *lib = (dt_map_t *)self->data;

  if(darktable.gui)
  {
    lib->image_pin = _init_image_pin();
    lib->place_pin = _init_place_pin();
    lib->drop_filmstrip_activated = FALSE;
    lib->thumb_lat_angle = 0.01, lib->thumb_lon_angle = 0.01;
    lib->loc.id = 0, lib->loc.location = NULL, lib->loc.time_out = 0;

    OsmGpsMapSource_t map_source
        = OSM_GPS_MAP_SOURCE_OPENSTREETMAP; // open street map should be a nice default ...
    gchar *old_map_source = dt_conf_get_string("plugins/map/map_source");
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
    g_free(old_map_source);

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

    /* allow drag&drop of images from filmstrip */
    gtk_drag_dest_set(GTK_WIDGET(lib->map), GTK_DEST_DEFAULT_ALL, target_list_internal, n_targets_internal,
                      GDK_ACTION_COPY);
    g_signal_connect(GTK_WIDGET(lib->map), "scroll-event", G_CALLBACK(_view_map_scroll_event), self);
    g_signal_connect(GTK_WIDGET(lib->map), "drag-data-received", G_CALLBACK(_drag_and_drop_received), self);
    g_signal_connect(GTK_WIDGET(lib->map), "changed", G_CALLBACK(_view_map_changed_callback), self);
    g_signal_connect_after(G_OBJECT(lib->map), "button-press-event",
                           G_CALLBACK(_view_map_button_press_callback), self);
    g_signal_connect(G_OBJECT(lib->map), "motion-notify-event", G_CALLBACK(_view_map_motion_notify_callback),
                     self);

    /* allow drag&drop of images from the map, too */
    g_signal_connect(GTK_WIDGET(lib->map), "drag-data-get", G_CALLBACK(_view_map_dnd_get_callback), self);
    g_signal_connect(GTK_WIDGET(lib->map), "drag-failed", G_CALLBACK(_view_map_dnd_failed_callback), self);
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
}

void cleanup(dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_view_map_collection_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_view_map_selection_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_view_map_check_preference_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_view_changed), self);

  if(darktable.gui)
  {
    g_object_unref(G_OBJECT(lib->image_pin));
    g_object_unref(G_OBJECT(lib->place_pin));
    g_object_unref(G_OBJECT(lib->osd));
    osm_gps_map_image_remove_all(lib->map);
    if(lib->images)
    {
      g_slist_free_full(lib->images, g_free);
      lib->images = NULL;
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

int try_enter(dt_view_t *self)
{
  return 0;
}

static void _view_map_signal_change_raise(gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_view_map_collection_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_GEOTAG_CHANGED, NULL);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_view_map_collection_changed), self);
}

// updating collection when mouse scrolls to resize the location is too demanding
// so wait for scrolling stop
static gboolean _signal_changes(gpointer user_data)
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
  _view_map_update_location_geotag(self);
  if(time_out)
  {
    if(lib->loc.time_out)
    {
      lib->loc.time_out = time_out;
    }
    else
    {
      lib->loc.time_out = time_out;
      g_timeout_add(100, _signal_changes, self);
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

// when map is moving we often get incorrect (even negative) values.
// keep the last positive values here to limit wrong effects (still not perfect)
static void _view_map_thumb_angles(dt_map_t *lib, const float lat0, const float lon0,
                                   float *dlat_min, float *dlon_min)
{
  OsmGpsMapPoint *pt0 = osm_gps_map_point_new_degrees(lat0, lon0);
  OsmGpsMapPoint *pt1 = osm_gps_map_point_new_degrees(0.0, 0.0);
  gint pixel_x = 0, pixel_y = 0;
  osm_gps_map_convert_geographic_to_screen(lib->map, pt0, &pixel_x, &pixel_y);
  osm_gps_map_convert_screen_to_geographic(lib->map, pixel_x + thumb_size,
                                           pixel_y + thumb_size, pt1);
  float lat1 = 0.0, lon1 = 0.0;
  osm_gps_map_point_get_degrees(pt1, &lat1, &lon1);
  osm_gps_map_point_free(pt0);
  osm_gps_map_point_free(pt1);
  *dlat_min = lat0 - lat1;
  *dlon_min = lon1 - lon0;
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

static void _view_map_draw_location(const dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  if(lib->loc.location)
  {
    osm_gps_map_image_remove(lib->map, lib->loc.location);
    lib->loc.location = NULL;
  }
  if(lib->loc.id)
  {
    const float del1 = _view_map_angles_to_pixels(lib, lib->loc.data.lat, lib->loc.data.lon,
                                                  lib->loc.data.delta1);
    const float del2 = del1 * lib->loc.data.delta2 / lib->loc.data.delta1;
    GdkPixbuf *shape = NULL;
    if(lib->loc.data.shape == MAP_LOCATION_SHAPE_CIRCLE)
      shape = _draw_circle(del1);
    else if(lib->loc.data.shape == MAP_LOCATION_SHAPE_RECTANGLE)
      shape = _draw_rectangle(del1, del2);

    if(shape)
    {
      lib->loc.location = osm_gps_map_image_add_with_alignment(lib->map,
                                                           lib->loc.data.lat,
                                                           lib->loc.data.lon,
                                                           shape, 0.5, 0.5);
      g_object_unref(shape);
    }
  }
}

static void _view_map_update_location_geotag(dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  if(lib->loc.id > 0)
  {
    // update coordinates
    dt_map_location_set_data(lib->loc.id, &lib->loc.data);
    dt_map_location_update_images(lib->loc.id);
  }
}

static void _view_map_changed_callback(OsmGpsMap *map, dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;

  /* check if the prefs have changed and rebuild main_query if needed */
  if(_view_map_prefs_changed(lib)) _view_map_build_main_query(lib);

  /* get bounding box coords */
  OsmGpsMapPoint bb[2];
  osm_gps_map_get_bbox(map, &bb[0], &bb[1]);
  float bb_0_lat = 0.0, bb_0_lon = 0.0, bb_1_lat = 0.0, bb_1_lon = 0.0;
  osm_gps_map_point_get_degrees(&bb[0], &bb_0_lat, &bb_0_lon);
  osm_gps_map_point_get_degrees(&bb[1], &bb_1_lat, &bb_1_lon);
  bb_0_lat = CLAMP(bb_0_lat, -90.0, 90.0);
  bb_1_lat = CLAMP(bb_1_lat, -90.0, 90.0);
  bb_0_lon = CLAMP(bb_0_lon, -180.0, 180.0);
  bb_1_lon = CLAMP(bb_1_lon, -180.0, 180.0);

  /* get map view state and store  */
  int zoom;
  float center_lat, center_lon;
  g_object_get(G_OBJECT(map), "zoom", &zoom, "latitude", &center_lat, "longitude", &center_lon, NULL);
  dt_conf_set_float("plugins/map/longitude", center_lon);
  dt_conf_set_float("plugins/map/latitude", center_lat);
  dt_conf_set_int("plugins/map/zoom", zoom);

  /* let's reset and reuse the main_query statement */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->main_query);
  DT_DEBUG_SQLITE3_RESET(lib->main_query);

  /* bind bounding box coords for the main query */
  DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->main_query, 1, bb_0_lon);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->main_query, 2, bb_1_lon);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->main_query, 3, bb_0_lat);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(lib->main_query, 4, bb_1_lat);

  /* remove the old images */
  if(lib->images)
  {
    // we can't use osm_gps_map_image_remove_all() because we want to keep the marker
    for(GSList *iter = lib->images; iter; iter = g_slist_next(iter))
    {
      dt_map_image_t *image = (dt_map_image_t *)iter->data;
      if(image->image) osm_gps_map_image_remove(map, image->image);
    }
    g_slist_free_full(lib->images, g_free);
    lib->images = NULL;
  }

  /* make the image list */
  gboolean needs_redraw = FALSE;
  const int _thumb_size = DT_PIXEL_APPLY_DPI(thumb_size);
  dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, _thumb_size, _thumb_size);
  gboolean all_good = TRUE;
  int img_count = 0;
  while(sqlite3_step(lib->main_query) == SQLITE_ROW && all_good)
  {
    const int imgid = sqlite3_column_int(lib->main_query, 0);
    dt_map_image_t *entry = (dt_map_image_t *)calloc(1, sizeof(dt_map_image_t));
    if(!entry)
      all_good = FALSE;
    else
    {
      entry->imgid = imgid;
      entry->latitude = sqlite3_column_double(lib->main_query, 1);
      entry->longitude = sqlite3_column_double(lib->main_query, 2);
      lib->images = g_slist_prepend(lib->images, entry);
      img_count++;
    }
  }

  // get the angles corresponding to thumbs
  float dlat_min, dlon_min;
  _view_map_thumb_angles(lib, center_lat, center_lon, &dlat_min, &dlon_min);
  // we would like to keep a small overlay
  dlat_min /= thumb_overlap;
  dlon_min /= thumb_overlap;

  if(all_good)
  {
    // set the groups
    const GList *sel_imgs = dt_view_get_images_to_act_on(TRUE, FALSE);
    for(GSList *iter = lib->images; iter; iter = g_slist_next(iter))
    {
      dt_map_image_t *entry = (dt_map_image_t *)iter->data;
      if(!entry->group)
      {
        entry->group = entry->imgid;
        entry->group_count = 1;
        entry->cum_lat = entry->latitude;
        entry->cum_lon = entry->longitude;
        entry->group_same_loc = TRUE;
        entry->selected_in_group = sel_imgs
                                   ? g_list_find((GList *)sel_imgs,
                                                 GINT_TO_POINTER(entry->imgid))
                                     ? TRUE : FALSE
                                   : FALSE;
        for(GSList *iter2 = iter; iter2; iter2 = g_slist_next(iter2))
        {
          dt_map_image_t *entry2 = (dt_map_image_t *)iter2->data;
          if(!entry2->group)
          {
            const float dlat = ABS(entry->latitude - entry2->latitude);
            const float dlon = ABS(entry->longitude - entry2->longitude);
            if(dlat <= dlat_min && dlon <= dlon_min)
            {
              entry2->group = entry->imgid;
              entry->group_count++;
              entry->cum_lat += entry2->latitude;
              entry->cum_lon += entry2->longitude;
              if(dlat != 0.0 || dlon != 0.0)
                entry->group_same_loc = FALSE;
              if(sel_imgs && !entry->selected_in_group)
                entry->selected_in_group = g_list_find((GList *)sel_imgs,
                                                       GINT_TO_POINTER(entry2->imgid))
                                           ? TRUE : FALSE;
            }
          }
        }
      }
    }
    int img_drawn = 0;
    for(GSList *iter = lib->images; iter; iter = g_slist_next(iter))
    {
      dt_map_image_t *entry = (dt_map_image_t *)iter->data;
      if(entry->imgid == entry->group && entry->group_count)
      {
        const int imgid = entry->imgid;
        dt_mipmap_buffer_t buf;
        dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, mip, DT_MIPMAP_BEST_EFFORT, 'r');

        if(buf.buf && buf.width > 0)
        {
          GdkPixbuf *source = NULL, *thumb = NULL, *count = NULL;

          for(size_t i = 3; i < (size_t)4 * buf.width * buf.height; i += 4) buf.buf[i] = UINT8_MAX;

          int w = _thumb_size, h = _thumb_size;
          const float _thumb_border = DT_PIXEL_APPLY_DPI(thumb_border);
          const float _pin_size = DT_PIXEL_APPLY_DPI(image_pin_size);
          if(buf.width < buf.height)
            w = (buf.width * _thumb_size) / buf.height; // portrait
          else
            h = (buf.height * _thumb_size) / buf.width; // landscape

          // next we get a pixbuf for the image
          source = gdk_pixbuf_new_from_data(buf.buf, GDK_COLORSPACE_RGB, TRUE,
                                            8, buf.width, buf.height,
                                            buf.width * 4, NULL, NULL);
          if(!source) goto map_changed_failure;

          // now we want a slightly larger pixbuf that we can put the image on
          thumb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w + 2 * _thumb_border,
                                 h + 2 * _thumb_border + _pin_size);
          if(!thumb) goto map_changed_failure;
          gdk_pixbuf_fill(thumb, entry->selected_in_group ? thumb_frame_sel_color
                                                          : thumb_frame_color);

          // put the image onto the frame
          gdk_pixbuf_scale(source, thumb, _thumb_border, _thumb_border, w, h,
                           _thumb_border, _thumb_border, (1.0 * w) / buf.width,
                           (1.0 * h) / buf.height, GDK_INTERP_HYPER);

          // add the pin
          gdk_pixbuf_copy_area(lib->image_pin, 0, 0, w + 2 * _thumb_border,
                               _pin_size, thumb, 0, h + 2 * _thumb_border);

          // add the count
          double count_height, count_width;
          count = _view_map_images_count(entry->group_count, entry->group_same_loc,
                                    &count_width, &count_height);
          gdk_pixbuf_copy_area(count, 0, 0, count_width, count_height, thumb,
                              _thumb_border, h - count_height + _thumb_border);

          entry->image = osm_gps_map_image_add_with_alignment(map,
                                            entry->cum_lat / entry->group_count,
                                            entry->cum_lon / entry->group_count,
                                            thumb, 0, 1);
          entry->width = w;
          entry->height = h;

        map_changed_failure:
          if(source) g_object_unref(source);
          if(thumb) g_object_unref(thumb);
          if(count) g_object_unref(count);
        }
        else
          needs_redraw = TRUE;
        dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
        img_drawn++;
      }
      // we limit the number of displayed images as required
      if(img_drawn >= lib->max_images_drawn)
        break;
    }
  }

  _view_map_draw_location(self);

  // not exactly thread safe, but should be good enough for updating the display
  static int timeout_event_source = 0;
  if(needs_redraw && timeout_event_source == 0)
    timeout_event_source = g_timeout_add_seconds(
        1, _view_map_redraw, self); // try again in a second, maybe some pictures have loaded by then
  else
    timeout_event_source = 0;

  // activate this callback late in the process as we need the filmstrip proxy to be setup. This is not the
  // case in the initialization phase.
  if(!lib->drop_filmstrip_activated)
  {
    g_signal_connect(dt_ui_thumbtable(darktable.gui->ui)->widget, "drag-data-received",
                     G_CALLBACK(_view_map_dnd_remove_callback), self);
    lib->drop_filmstrip_activated = TRUE;
  }
}

static GList *_view_map_get_imgs_at_pos(dt_view_t *self, const double x,
                                        const double y, const gboolean first_on)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  GList *imgs = NULL;
  GSList *iter_imgs = NULL;

  for(GSList *iter = lib->images; iter != NULL; iter = iter->next)
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
        imgs = g_list_append(imgs,GINT_TO_POINTER(entry->imgid));
        iter_imgs = iter;
        break;
      }
    }
  }

  if(iter_imgs && !first_on)
  {
    dt_map_image_t *entry = (dt_map_image_t *)iter_imgs->data;
    const int imgid = GPOINTER_TO_INT(imgs->data);
    if(entry->group_count > 1)
    {
      for(GSList *iter = iter_imgs->next; iter != NULL; iter = iter->next)
      {
        entry = (dt_map_image_t *)iter->data;
        if(entry->group == imgid)
        {
          // prepend is faster but we need the visible image at first place
          imgs = g_list_append(imgs,GINT_TO_POINTER(entry->imgid));
        }
      }
    }
  }

  return imgs;
}

static gboolean _view_map_motion_notify_callback(GtkWidget *widget, GdkEventMotion *e, dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;

  if(lib->loc.drag)
  {
    if(lib->loc.id > 0)
    {
      lib->loc.drag = FALSE;
      osm_gps_map_image_remove(lib->map, lib->loc.location);
      lib->loc.drag = FALSE;
      GtkTargetList *targets = gtk_target_list_new(target_list_internal, n_targets_internal);

      GdkDragContext *context =
        gtk_drag_begin_with_coordinates(GTK_WIDGET(lib->map), targets,
                                        GDK_ACTION_COPY, 1, (GdkEvent *)e, -1, -1);

      GdkPixbuf *cross = _draw_cross();
      gtk_drag_set_icon_pixbuf(context, cross, DT_PIXEL_APPLY_DPI(cross_size),
                               DT_PIXEL_APPLY_DPI(cross_size));
      g_object_unref(cross);
      gtk_target_list_unref(targets);
      return TRUE;
      }
  }

  if(lib->start_drag && lib->selected_images)
  {
    for(GSList *iter = lib->images; iter != NULL; iter = iter->next)
    {
      dt_map_image_t *entry = (dt_map_image_t *)iter->data;
      OsmGpsMapImage *image = entry->image;
      if(image)
      {
        GList *sel_img = lib->selected_images;
        if(entry->imgid == GPOINTER_TO_INT(sel_img->data) &&
           (sel_img->next || (!sel_img->next && entry->group_count == 1)))
        {
          // keep the image on map if only the first of a group is moved
          // TODO display instead the thumb of the next remaining image if any
          osm_gps_map_image_remove(lib->map, image);
        }
      }
    }

    int group_count = 0;
    for(GList *iter = lib->selected_images; iter != NULL; iter = iter->next)
    {
      group_count++;
    }

    lib->start_drag = FALSE;
    GtkTargetList *targets = gtk_target_list_new(target_list_all, n_targets_all);

    // FIXME: for some reason the image is only shown when it's above a certain size,
    // which happens to be > than the normal-DPI one. When dragging from filmstrip it works though.
    const int _thumb_size = DT_PIXEL_APPLY_DPI(thumb_size);
    dt_mipmap_buffer_t buf;
    dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, _thumb_size, _thumb_size);
    dt_mipmap_cache_get(darktable.mipmap_cache, &buf,
                        GPOINTER_TO_INT(lib->selected_images->data),
                        mip, DT_MIPMAP_BLOCKING, 'r');

    if(buf.buf && buf.width > 0)
    {
      GdkPixbuf *source = NULL, *thumb = NULL, *count = NULL;

      for(size_t i = 3; i < (size_t)4 * buf.width * buf.height; i += 4) buf.buf[i] = UINT8_MAX;

      int w = _thumb_size, h = _thumb_size;
      const float _thumb_border = DT_PIXEL_APPLY_DPI(thumb_border);
      if(buf.width < buf.height)
        w = (buf.width * _thumb_size) / buf.height; // portrait
      else
        h = (buf.height * _thumb_size) / buf.width; // landscape
      // next we get a pixbuf for the image
      source = gdk_pixbuf_new_from_data(buf.buf, GDK_COLORSPACE_RGB, TRUE, 8, buf.width, buf.height,
                                        buf.width * 4, NULL, NULL);

      // now we want a slightly larger pixbuf that we can put the image on
      thumb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w + 2 * _thumb_border, h + 2 * _thumb_border);
      gdk_pixbuf_fill(thumb, thumb_frame_color);

      // put the image onto the frame
      gdk_pixbuf_scale(source, thumb, _thumb_border, _thumb_border, w, h, _thumb_border, _thumb_border,
                       (1.0 * w) / buf.width, (1.0 * h) / buf.height, GDK_INTERP_HYPER);

      // add the count
      double count_height, count_width;
      count = _view_map_images_count(group_count, TRUE, &count_width, &count_height);
      gdk_pixbuf_copy_area(count, 0, 0, count_width, count_height, thumb, _thumb_border,
                           h - count_height + _thumb_border);

      GdkDragContext *context = gtk_drag_begin_with_coordinates(GTK_WIDGET(lib->map), targets,
                                                                GDK_ACTION_COPY, 1, (GdkEvent *)e, -1, -1);

      gtk_drag_set_icon_pixbuf(context, thumb, 0, h + 2 * _thumb_border);

      if(source) g_object_unref(source);
      if(thumb) g_object_unref(thumb);
      if(count) g_object_unref(count);
    }

    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

    gtk_target_list_unref(targets);
    return TRUE;
  }
  return FALSE;
}

static gboolean _view_map_scroll_event(GtkWidget *w, GdkEventScroll *event, dt_view_t *self)
{
  dt_map_t *lib = (dt_map_t *)self->data;
  if(lib->loc.id > 0)
  {
    OsmGpsMapPoint *p = osm_gps_map_get_event_location(lib->map, (GdkEventButton *)event);
    float lat, lon;
    osm_gps_map_point_get_degrees(p, &lat, &lon);
    if(dt_map_location_included(lon, lat, &lib->loc.data))
    {
      if(lib->loc.data.shape == MAP_LOCATION_SHAPE_RECTANGLE &&
        (event->state & GDK_SHIFT_MASK))
      {
        if(event->direction == GDK_SCROLL_DOWN)
          lib->loc.data.delta1 *= 1.1;
        else
          lib->loc.data.delta1 /= 1.1;
      }
      else if(lib->loc.data.shape == MAP_LOCATION_SHAPE_RECTANGLE &&
             (event->state & GDK_CONTROL_MASK))
      {
        if(event->direction == GDK_SCROLL_DOWN)
          lib->loc.data.delta2 *= 1.1;
        else
          lib->loc.data.delta2 /= 1.1;
      }
      else
      {
        if(event->direction == GDK_SCROLL_DOWN)
        {
          lib->loc.data.delta1 *= 1.1;
          lib->loc.data.delta2 *= 1.1;
        }
        else
        {
          lib->loc.data.delta1 /= 1.1;
          lib->loc.data.delta2 /= 1.1;
        }
      }
      _view_map_draw_location(self);
      _view_map_signal_change_wait(self, 5);  // wait 5/10 sec after last scroll
      return TRUE;
    }
    else  // scroll on the map. try to keep the map where it is
    {
      if(event->direction == GDK_SCROLL_UP)
      {
        osm_gps_map_zoom_in(lib->map);
      }
      else
      {
        osm_gps_map_zoom_out(lib->map);
      }
      return TRUE;
    }
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
    // check if the click was in a location circle
    if(lib->loc.id > 0 && !(e->state & GDK_CONTROL_MASK))
    {
      OsmGpsMapPoint *p = osm_gps_map_get_event_location(lib->map, e);
      float lat, lon;
      osm_gps_map_point_get_degrees(p, &lat, &lon);
      if(dt_map_location_included(lon, lat, &lib->loc.data))
      {
        lib->loc.drag = TRUE;
        return TRUE;
      }
    }
    // check if the click was on image(s) or just some random position
    lib->selected_images = _view_map_get_imgs_at_pos(self, e->x, e->y, TRUE);
    if(e->type == GDK_BUTTON_PRESS)
    {
      if(e->state & GDK_SHIFT_MASK)
      {
        lib->selected_images = _view_map_get_imgs_at_pos(self, e->x, e->y, FALSE);
      }
      if(lib->selected_images)
      {
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
  darktable.view_manager->proxy.map.view = self;
  darktable.view_manager->proxy.map.center_on_location = _view_map_center_on_location;
  darktable.view_manager->proxy.map.center_on_bbox = _view_map_center_on_bbox;
  darktable.view_manager->proxy.map.show_osd = _view_map_show_osd;
  darktable.view_manager->proxy.map.set_map_source = _view_map_set_map_source;
  darktable.view_manager->proxy.map.add_marker = _view_map_add_marker;
  darktable.view_manager->proxy.map.remove_marker = _view_map_remove_marker;
  darktable.view_manager->proxy.map.add_location = _view_map_add_location;
  darktable.view_manager->proxy.map.remove_location= _view_map_remove_location;
  darktable.view_manager->proxy.map.redraw = _view_map_redraw;
  darktable.view_manager->proxy.map.display_selected = _view_map_display_selected;

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

void init_key_accels(dt_view_t *self)
{
  dt_accel_register_view(self, NC_("accel", "undo"), GDK_KEY_z, GDK_CONTROL_MASK);
  dt_accel_register_view(self, NC_("accel", "redo"), GDK_KEY_y, GDK_CONTROL_MASK);
}

static gboolean _view_map_undo_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                        GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_map_t *lib = (dt_map_t *)self->data;

  // let current map view unchanged (avoid to center the map on collection)
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_view_map_collection_changed), data);
  dt_undo_do_undo(darktable.undo, DT_UNDO_MAP);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_view_map_collection_changed), data);
  g_signal_emit_by_name(lib->map, "changed");

  return TRUE;
}

static gboolean _view_map_redo_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                        GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_map_t *lib = (dt_map_t *)self->data;

  // let current map view unchanged (avoid to center the map on collection)
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_view_map_collection_changed), data);
  dt_undo_do_redo(darktable.undo, DT_UNDO_MAP);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_view_map_collection_changed), data);
  g_signal_emit_by_name(lib->map, "changed");

  return TRUE;
}

void connect_key_accels(dt_view_t *self)
{
  // undo/redo
  GClosure *closure = g_cclosure_new(G_CALLBACK(_view_map_undo_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "undo", closure);
  closure = g_cclosure_new(G_CALLBACK(_view_map_redo_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "redo", closure);
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

static void _view_map_show_osd(const dt_view_t *view, gboolean enabled)
{
  dt_map_t *lib = (dt_map_t *)view->data;

  gboolean old_value = dt_conf_get_bool("plugins/map/show_map_osd");
  if(enabled == old_value) return;

  dt_conf_set_bool("plugins/map/show_map_osd", enabled);
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

#ifdef HAVE_OSMGPSMAP_110_OR_NEWER
static OsmGpsMapPolygon *_view_map_add_polygon(const dt_view_t *view, GList *points)
{
  dt_map_t *lib = (dt_map_t *)view->data;

  OsmGpsMapPolygon *poly = osm_gps_map_polygon_new();
  OsmGpsMapTrack* track = osm_gps_map_track_new();

  for(GList *iter = g_list_first(points); iter; iter = g_list_next(iter))
  {
    dt_geo_map_display_point_t *p = (dt_geo_map_display_point_t *)iter->data;
    OsmGpsMapPoint* point = osm_gps_map_point_new_degrees(p->lat, p->lon);
    osm_gps_map_track_add_point(track, point);
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

  for(GList *iter = g_list_first(points); iter; iter = g_list_next(iter))
  {
    dt_geo_map_display_point_t *p = (dt_geo_map_display_point_t *)iter->data;
    OsmGpsMapPoint* point = osm_gps_map_point_new_degrees(p->lat, p->lon);
    osm_gps_map_track_add_point(track, point);
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

static GObject *_view_map_add_marker(const dt_view_t *view, dt_geo_map_display_t type, GList *points)
{
  switch(type)
  {
    case MAP_DISPLAY_POINT: return G_OBJECT(_view_map_add_pin(view, points));
    case MAP_DISPLAY_TRACK: return G_OBJECT(_view_map_add_track(view, points));
#ifdef HAVE_OSMGPSMAP_110_OR_NEWER
    case MAP_DISPLAY_POLYGON: return G_OBJECT(_view_map_add_polygon(view, points));
#endif
    default: return NULL;
  }
}

static gboolean _view_map_remove_marker(const dt_view_t *view, dt_geo_map_display_t type, GObject *marker)
{
  if(type == MAP_DISPLAY_NONE) return FALSE;

  switch(type)
  {
    case MAP_DISPLAY_POINT: return _view_map_remove_pin(view, OSM_GPS_MAP_IMAGE(marker));
    case MAP_DISPLAY_TRACK: return _view_map_remove_track(view, OSM_GPS_MAP_TRACK(marker));
#ifdef HAVE_OSMGPSMAP_110_OR_NEWER
    case MAP_DISPLAY_POLYGON: return _view_map_remove_polygon(view, OSM_GPS_MAP_POLYGON(marker));
#endif
    default: return FALSE;
  }
}

static void _view_map_add_location(const dt_view_t *view, dt_map_location_data_t *g, const guint locid)
{
  dt_map_t *lib = (dt_map_t *)view->data;
  lib->loc.id = locid;
  if(g)
  {
    if(g->delta1 != 0.0 && g->delta2 != 0.0)
    {
      // existing location
      memcpy(&lib->loc.data, g, sizeof(dt_map_location_data_t));
      int zoom;
      g_object_get(G_OBJECT(lib->map), "zoom", &zoom, NULL);

      const float max_lon = CLAMP(g->lon + g->delta1, -180, 180);
      const float min_lon = CLAMP(g->lon - g->delta1, -180, 180);
      const float max_lat = CLAMP(g->lat + g->delta2, -90, 90);
      const float min_lat = CLAMP(g->lat - g->delta2, -90, 90);
      if(max_lon > min_lon && max_lat > min_lat)
      {
        _view_map_center_on_bbox(view, min_lon, min_lat, max_lon, max_lat);
        _view_map_draw_location(view);
      }
    }
    else
    {
      // this is a new location
      lib->loc.data.shape = g->shape;
      int zoom;
      float lon, lat;
      g_object_get(G_OBJECT(lib->map), "zoom", &zoom,
                   "latitude", &lat, "longitude", &lon, NULL);
      lib->loc.data.lon = lon, lib->loc.data.lat = lat;
      // get a radius angle equivalent to thumb dimension to start with
      float dlat, dlon;
      _view_map_thumb_angles(lib, lib->loc.data.lat, lib->loc.data.lon, &dlat, &dlon);
      lib->loc.data.delta1 = lib->loc.data.delta2 = dlat;
      _view_map_draw_location(view);
      _view_map_signal_change_wait((dt_view_t *)view, 1);
    }
  }
}

static void _view_map_remove_location(const dt_view_t *view)
{
  dt_map_t *lib = (dt_map_t *)view->data;
  lib->loc.id = 0;
  if(lib->loc.location)
  {
    osm_gps_map_image_remove(lib->map, lib->loc.location);
  }
  lib->loc.location = NULL;
}


static void _view_map_check_preference_changed(gpointer instance, gpointer user_data)
{
  dt_view_t *view = (dt_view_t *)user_data;
  dt_map_t *lib = (dt_map_t *)view->data;

  if(_view_map_prefs_changed(lib)) g_signal_emit_by_name(lib->map, "changed");
}

static void _view_map_collection_changed(gpointer instance, dt_collection_change_t query_change, gpointer imgs,
                                         int next, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_map_t *lib = (dt_map_t *)self->data;
  // avoid to centre the map on collection while a location is active
  if(darktable.view_manager->proxy.map.view && !lib->loc.id)
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

static void _view_map_center_on_image(dt_view_t *self, const int32_t imgid)
{
  if(imgid)
  {
    const dt_map_t *lib = (dt_map_t *)self->data;
    dt_image_geoloc_t geoloc;
    dt_image_get_location(imgid, &geoloc);

    if(!isnan(geoloc.longitude) && !isnan(geoloc.latitude))
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
  double max_longitude = -INFINITY;
  double max_latitude = -INFINITY;
  double min_longitude = INFINITY;
  double min_latitude = INFINITY;
  int count = 0;

  char *query = dt_util_dstrcat(NULL,
                                "SELECT MIN(latitude), MAX(latitude),"
                                "       MIN(longitude), MAX(longitude), COUNT(*)"
                                " FROM main.images AS i "
                                " JOIN %s AS l ON l.imgid = i.id "
                                " WHERE latitude NOT NULL AND longitude NOT NULL",
                                table);
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

static void _view_map_filmstrip_activate_callback(gpointer instance, int imgid, gpointer user_data)
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
        lib->loc.data.lat = lat, lib->loc.data.lon = lon;
        osm_gps_map_point_free(pt);
        _view_map_signal_change_wait(self, 0);
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
        osm_gps_map_convert_screen_to_geographic(lib->map, x, y, pt);
        osm_gps_map_point_get_degrees(pt, &latitude, &longitude);
        osm_gps_map_point_free(pt);
        const dt_image_geoloc_t geoloc = { longitude, latitude, NAN };
        dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_view_map_collection_changed), self);
        dt_image_set_locations(imgs, &geoloc, TRUE);
        dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_view_map_collection_changed), self);
        g_list_free(imgs);
        success = TRUE;
      }
    }
  }
  gtk_drag_finish(context, success, FALSE, time);
  if(success) g_signal_emit_by_name(lib->map, "changed");
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
          const int imgs_nb = g_list_length(lib->selected_images);
          if(imgs_nb)
          {
            uint32_t *imgs = malloc(imgs_nb * sizeof(uint32_t));
            GList *l = lib->selected_images;
            for(int i = 0; i < imgs_nb; i++)
            {
              imgs[i] = GPOINTER_TO_INT(l->data);
              l = g_list_next(l);
            }
            gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data),
                                   _DWORD, (guchar *)imgs, imgs_nb * sizeof(uint32_t));
            free(imgs);
          }
        }
        else if(lib->loc.id > 0)
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
        int imgid = GPOINTER_TO_INT(lib->selected_images->data);
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
      const dt_image_geoloc_t geoloc = { NAN, NAN, NAN };
      dt_image_set_locations(imgs, &geoloc, TRUE);
      g_list_free(imgs);
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
  int max_images_drawn = dt_conf_get_int("plugins/map/max_images_drawn");
  gboolean filter_images_drawn = dt_conf_get_bool("plugins/map/filter_images_drawn");

  if(lib->max_images_drawn != max_images_drawn) prefs_changed = TRUE;
  if(lib->filter_images_drawn != filter_images_drawn) prefs_changed = TRUE;

  return prefs_changed;
}

static void _view_map_build_main_query(dt_map_t *lib)
{
  char *geo_query;

  if(lib->main_query) sqlite3_finalize(lib->main_query);

  lib->max_images_drawn = dt_conf_get_int("plugins/map/max_images_drawn");
  if(lib->max_images_drawn == 0) lib->max_images_drawn = 100;
  lib->filter_images_drawn = dt_conf_get_bool("plugins/map/filter_images_drawn");
  geo_query = g_strdup_printf("SELECT * FROM"
                              " (SELECT id, latitude, longitude "
                              "   FROM %s WHERE longitude >= ?1 AND longitude <= ?2"
                              "           AND latitude <= ?3 AND latitude >= ?4 "
                              "           AND longitude NOT NULL AND latitude NOT NULL"
                              "   ORDER BY latitude DESC, longitude DESC) ",
                              lib->filter_images_drawn
                              ? "main.images i INNER JOIN memory.collected_images c ON i.id = c.imgid"
                              : "main.images");

  /* prepare the main query statement */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), geo_query, -1, &lib->main_query, NULL);

  g_free(geo_query);
}

GSList *mouse_actions(const dt_view_t *self)
{
  GSList *lm = NULL;
  dt_mouse_action_t *a = NULL;

  a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
  a->action = DT_MOUSE_ACTION_DOUBLE_LEFT;
  g_strlcpy(a->name, _("[on image] open in darkroom"), sizeof(a->name));
  lm = g_slist_append(lm, a);

  a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
  a->action = DT_MOUSE_ACTION_DOUBLE_LEFT;
  g_strlcpy(a->name, _("[on map] zoom map"), sizeof(a->name));
  lm = g_slist_append(lm, a);

  a = (dt_mouse_action_t *)calloc(1, sizeof(dt_mouse_action_t));
  a->action = DT_MOUSE_ACTION_DRAG_DROP;
  g_strlcpy(a->name, _("move image location"), sizeof(a->name));
  lm = g_slist_append(lm, a);

  return lm;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
