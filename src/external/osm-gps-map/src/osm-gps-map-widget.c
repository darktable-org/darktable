/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4; tab-width: 4 -*- */
/* vim:set et sw=4 ts=4 */
/*
 * Copyright (C) 2013 John Stowers <john.stowers@gmail.com>
 * Copyright (C) Marcus Bauer 2008 <marcus.bauer@gmail.com>
 * Copyright (C) John Stowers 2009 <john.stowers@gmail.com>
 * Copyright (C) Till Harbaum 2009 <till@harbaum.org>
 *
 * Contributions by
 * Everaldo Canuto 2009 <everaldo.canuto@gmail.com>
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
 * SECTION:osm-gps-map
 * @short_description: The map display widget
 * @stability: Stable
 * @include: osm-gps-map.h
 *
 * #OsmGpsMap is a widget for displaying a map, optionally overlaid with a
 * track(s) of GPS co-ordinates, images, points of interest or on screen display
 * controls. #OsmGpsMap downloads (and caches for offline use) map data from a
 * number of websites, including
 * <ulink url="http://www.openstreetmap.org"><citetitle>OpenStreetMap</citetitle></ulink>
 *
 * <example>
 *  <title>Showing a map</title>
 *  <programlisting>
 * int main (int argc, char **argv)
 * {
 *     g_thread_init(NULL);
 *     gtk_init (&argc, &argv);
 *
 *     GtkWidget *map = osm_gps_map_new ();
 *     GtkWidget *w = gtk_window_new (GTK_WINDOW_TOPLEVEL);
 *     gtk_container_add (GTK_CONTAINER(w), map);
 *     gtk_widget_show_all (w);
 *
 *     gtk_main ();
 *     return 0;
 * }
 *  </programlisting>
 * </example>
 *
 * #OsmGpsMap allows great flexibility in customizing how the map tiles are
 * cached, see #OsmGpsMap:tile-cache-base and #OsmGpsMap:tile-cache for more
 * information.
 *
 * A number of different map sources are supported, see #OsmGpsMapSource_t. The
 * default source, %OSM_GPS_MAP_SOURCE_OPENSTREETMAP always works. Other sources,
 * particular those from proprietary providers may work occasionally, and then
 * cease to work. To check if a source is supported for the given version of
 * this library, call osm_gps_map_source_is_valid().
 *
 * <example>
 *  <title>Map with custom source and cache dir</title>
 *  <programlisting>
 * int main (int argc, char **argv)
 * {
 *     g_thread_init(NULL);
 *     gtk_init (&argc, &argv);
 *     OsmGpsMapSource_t source = OSM_GPS_MAP_SOURCE_VIRTUAL_EARTH_SATELLITE;
 *
 *     if ( !osm_gps_map_source_is_valid(source) )
 *         return 1;
 *
 *     GtkWidget *map = g_object_new (OSM_TYPE_GPS_MAP,
 *                      "map-source", source,
 *                      "tile-cache", "/tmp/",
 *                       NULL);
 *     GtkWidget *w = gtk_window_new (GTK_WINDOW_TOPLEVEL);
 *     gtk_container_add (GTK_CONTAINER(w), map);
 *     gtk_widget_show_all (w);
 *
 *     gtk_main ();
 *     return 0;
 * }
 *  </programlisting>
 * </example>
 *
 * Finally, if you wish to use a custom map source not supported by #OsmGpsMap, 
 * such as a custom map created with
 * <ulink url="http://www.cloudmade.com"><citetitle>CloudMade</citetitle></ulink>
 * then you can also pass a specially formatted string to #OsmGpsMap:repo-uri.
 *
 * <example>
 *  <title>Map using custom CloudMade map and on screen display</title>
 *  <programlisting>
 * int main (int argc, char **argv)
 * {
 *     g_thread_init(NULL);
 *     gtk_init (&argc, &argv);
 *     const gchar *cloudmate = "http://a.tile.cloudmade.com/YOUR_API_KEY/1/256/&num;Z/&num;X/&num;Y.png";
 *
 *     GtkWidget *map = g_object_new (OSM_TYPE_GPS_MAP,
 *                      "repo-uri", cloudmate,
 *                       NULL);
 *     OsmGpsMapOsd *osd = osm_gps_map_osd_new ();
 *     GtkWidget *w = gtk_window_new (GTK_WINDOW_TOPLEVEL);
 *     osm_gps_map_layer_add (OSM_GPS_MAP(map), OSM_GPS_MAP_LAYER(osd));
 *     gtk_container_add (GTK_CONTAINER(w), map);
 *     gtk_widget_show_all (w);
 *
 *     gtk_main ();
 *     return 0;
 * }
 *  </programlisting>
 * </example>
 **/

#include "config.h"

#include <fcntl.h>
#include <math.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gdk/gdk.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gprintf.h>
#include <libsoup/soup.h>

#include "converter.h"
#include "private.h"
#include "osm-gps-map-source.h"
#include "osm-gps-map-widget.h"
#include "osm-gps-map-compat.h"

#define ENABLE_DEBUG                (0)
#define EXTRA_BORDER                (0)
#define OSM_GPS_MAP_SCROLL_STEP     (10)
#define USER_AGENT                  "libosmgpsmap/1.0"
#define DOWNLOAD_RETRIES            3
#define MAX_DOWNLOAD_TILES          10000

struct _OsmGpsMapPrivate
{
    GHashTable *tile_queue;
    GHashTable *missing_tiles;
    GHashTable *tile_cache;

    int map_zoom;
    int max_zoom;
    int min_zoom;

    int tile_zoom_offset;

    int map_x;
    int map_y;

    /* Controls auto centering the map when a new GPS position arrives */
    gfloat map_auto_center_threshold;

    /* Latitude and longitude of the center of the map, in radians */
    gfloat center_rlat;
    gfloat center_rlon;

    guint max_tile_cache_size;
    /* Incremented at each redraw */
    guint redraw_cycle;
    /* ID of the idle redraw operation */
    guint idle_map_redraw;

    //how we download tiles
    SoupSession *soup_session;
    char *proxy_uri;

    //where downloaded tiles are cached
    char *tile_dir;
    char *tile_base_dir;
    char *cache_dir;

    //contains flags indicating the various special characters
    //the uri string contains, that will be replaced when calculating
    //the uri to download.
    OsmGpsMapSource_t map_source;
    char *repo_uri;
    char *image_format;
    int uri_format;

    //gps tracking state
    GSList *trip_history;
    float gps_heading;

    OsmGpsMapPoint *gps;
    OsmGpsMapTrack *gps_track;
    gboolean gps_track_used;

    //additional images or tracks added to the map
    GSList *tracks;
    GSList *images;

    //Used for storing the joined tiles
    cairo_surface_t *pixmap;

    //The tile painted when one cannot be found
    GdkPixbuf *null_tile;

    //A list of OsmGpsMapLayer* layers, such as the OSD
    GSList *layers;

    //For tracking click and drag
    int drag_counter;
    int drag_mouse_dx;
    int drag_mouse_dy;
    int drag_start_mouse_x;
    int drag_start_mouse_y;
    int drag_start_map_x;
    int drag_start_map_y;
    int drag_limit;
    guint drag_expose_source;

    /* for customizing the redering of the gps track */
    int ui_gps_point_inner_radius;
    int ui_gps_point_outer_radius;

    /* For storing keybindings */
    guint keybindings[OSM_GPS_MAP_KEY_MAX];

    /* flags controlling which features are enabled */
    guint keybindings_enabled : 1;
    guint map_auto_download_enabled : 1;
    guint map_auto_center_enabled : 1;
    guint trip_history_record_enabled : 1;
    guint trip_history_show_enabled : 1;
    guint gps_point_enabled : 1;

    /* state flags */
    guint is_disposed : 1;
    guint is_constructed : 1;
    guint is_dragging : 1;
    guint is_button_down : 1;
    guint is_fullscreen : 1;
    guint is_google : 1;
};

typedef struct
{
    GdkPixbuf *pixbuf;
    /* We keep track of the number of the redraw cycle this tile was last used,
     * so that osm_gps_map_purge_cache() can remove the older ones */
    guint redraw_cycle;
} OsmCachedTile;

typedef struct {
    /* The details of the tile to download */
    char *uri;
    char *folder;
    char *filename;
    OsmGpsMap *map;
    /* whether to redraw the map when the tile arrives */
    gboolean redraw;
    int ttl;
} OsmTileDownload;

enum
{
    PROP_0,
    PROP_AUTO_CENTER,
    PROP_RECORD_TRIP_HISTORY,
    PROP_SHOW_TRIP_HISTORY,
    PROP_AUTO_DOWNLOAD,
    PROP_REPO_URI,
    PROP_PROXY_URI,
    PROP_TILE_CACHE_DIR,
    PROP_TILE_CACHE_BASE_DIR,
    PROP_TILE_ZOOM_OFFSET,
    PROP_ZOOM,
    PROP_MAX_ZOOM,
    PROP_MIN_ZOOM,
    PROP_LATITUDE,
    PROP_LONGITUDE,
    PROP_MAP_X,
    PROP_MAP_Y,
    PROP_TILES_QUEUED,
    PROP_GPS_TRACK_WIDTH,
    PROP_GPS_POINT_R1,
    PROP_GPS_POINT_R2,
    PROP_MAP_SOURCE,
    PROP_IMAGE_FORMAT,
    PROP_DRAG_LIMIT,
    PROP_AUTO_CENTER_THRESHOLD,
    PROP_SHOW_GPS_POINT
};

G_DEFINE_TYPE (OsmGpsMap, osm_gps_map, GTK_TYPE_DRAWING_AREA);

/*
 * Drawing function forward defintions
 */
static gchar    *replace_string(const gchar *src, const gchar *from, const gchar *to);
static gchar    *replace_map_uri(OsmGpsMap *map, const gchar *uri, int zoom, int x, int y);
static void     osm_gps_map_tile_download_complete (SoupSession *session, SoupMessage *msg, gpointer user_data);
static void     osm_gps_map_download_tile (OsmGpsMap *map, int zoom, int x, int y, gboolean redraw);
static gboolean osm_gps_map_map_redraw (OsmGpsMap *map);
static void     osm_gps_map_map_redraw_idle (OsmGpsMap *map);
static GdkPixbuf* osm_gps_map_render_tile_upscaled (OsmGpsMap *map, GdkPixbuf *tile, int tile_zoom, int zoom, int x, int y);

static void
cached_tile_free (OsmCachedTile *tile)
{
    g_object_unref (tile->pixbuf);
    g_slice_free (OsmCachedTile, tile);
}

/*
 * Description:
 *   Find and replace text within a string.
 *
 * Parameters:
 *   src  (in) - pointer to source string
 *   from (in) - pointer to search text
 *   to   (in) - pointer to replacement text
 *
 * Returns:
 *   Returns a pointer to dynamically-allocated memory containing string
 *   with occurences of the text pointed to by 'from' replaced by with the
 *   text pointed to by 'to'.
 */
static gchar *
replace_string(const gchar *src, const gchar *from, const gchar *to)
{
    size_t size    = strlen(src) + 1;
    size_t fromlen = strlen(from);
    size_t tolen   = strlen(to);

    /* Allocate the first chunk with enough for the original string. */
    gchar *value = g_malloc(size);


    /* We need to return 'value', so let's make a copy to mess around with. */
    gchar *dst = value;

    if ( value != NULL )
    {
        for ( ;; )
        {
            /* Try to find the search text. */
            const gchar *match = g_strstr_len(src, size, from);
            if ( match != NULL )
            {
                gchar *temp;
                /* Find out how many characters to copy up to the 'match'. */
                size_t count = match - src;


                /* Calculate the total size the string will be after the
                 * replacement is performed. */
                size += tolen - fromlen;

                temp = g_realloc(value, size);
                if ( temp == NULL )
                {
                    g_free(value);
                    return NULL;
                }

                /* we'll want to return 'value' eventually, so let's point it
                 * to the memory that we are now working with.
                 * And let's not forget to point to the right location in
                 * the destination as well. */
                dst = temp + (dst - value);
                value = temp;

                /*
                 * Copy from the source to the point where we matched. Then
                 * move the source pointer ahead by the amount we copied. And
                 * move the destination pointer ahead by the same amount.
                 */
                g_memmove(dst, src, count);
                src += count;
                dst += count;

                /* Now copy in the replacement text 'to' at the position of
                 * the match. Adjust the source pointer by the text we replaced.
                 * Adjust the destination pointer by the amount of replacement
                 * text. */
                g_memmove(dst, to, tolen);
                src += fromlen;
                dst += tolen;
            }
            else
            {
                /*
                 * Copy any remaining part of the string. This includes the null
                 * termination character.
                 */
                strcpy(dst, src);
                break;
            }
        }
    }
    return value;
}

static void
map_convert_coords_to_quadtree_string(OsmGpsMap *map, gint x, gint y, gint zoomlevel,
                                      gchar *buffer, const gchar initial,
                                      const gchar *const quadrant)
{
    gchar *ptr = buffer;
    gint n;

    if (initial)
        *ptr++ = initial;

    for(n = zoomlevel-1; n >= 0; n--)
    {
        gint xbit = (x >> n) & 1;
        gint ybit = (y >> n) & 1;
        *ptr++ = quadrant[xbit + 2 * ybit];
    }

    *ptr++ = '\0';
}


static void
inspect_map_uri(OsmGpsMapPrivate *priv)
{
    priv->uri_format = 0;
    priv->is_google = FALSE;

    if (g_strrstr(priv->repo_uri, URI_MARKER_X))
        priv->uri_format |= URI_HAS_X;

    if (g_strrstr(priv->repo_uri, URI_MARKER_Y))
        priv->uri_format |= URI_HAS_Y;

    if (g_strrstr(priv->repo_uri, URI_MARKER_Z))
        priv->uri_format |= URI_HAS_Z;

    if (g_strrstr(priv->repo_uri, URI_MARKER_S))
        priv->uri_format |= URI_HAS_S;

    if (g_strrstr(priv->repo_uri, URI_MARKER_Q))
        priv->uri_format |= URI_HAS_Q;

    if (g_strrstr(priv->repo_uri, URI_MARKER_Q0))
        priv->uri_format |= URI_HAS_Q0;

    if (g_strrstr(priv->repo_uri, URI_MARKER_YS))
        priv->uri_format |= URI_HAS_YS;

    if (g_strrstr(priv->repo_uri, URI_MARKER_R))
        priv->uri_format |= URI_HAS_R;

    if (g_strrstr(priv->repo_uri, "google.com"))
        priv->is_google = TRUE;

    g_debug("URI Format: 0x%X (google: %X)", priv->uri_format, priv->is_google);

}

static gchar *
replace_map_uri(OsmGpsMap *map, const gchar *uri, int zoom, int x, int y)
{
    OsmGpsMapPrivate *priv = map->priv;
    char *url;
    unsigned int i;
    char location[22];

    i = 1;
    url = g_strdup(uri);
    while (i < URI_FLAG_END)
    {
        char *s = NULL;
        char *old;

        old = url;
        switch(i & priv->uri_format)
        {
            case URI_HAS_X:
                s = g_strdup_printf("%d", x);
                url = replace_string(url, URI_MARKER_X, s);
                break;
            case URI_HAS_Y:
                s = g_strdup_printf("%d", y);
                url = replace_string(url, URI_MARKER_Y, s);
                break;
            case URI_HAS_Z:
                s = g_strdup_printf("%d", zoom);
                url = replace_string(url, URI_MARKER_Z, s);
                break;
            case URI_HAS_S:
                s = g_strdup_printf("%d", priv->max_zoom-zoom);
                url = replace_string(url, URI_MARKER_S, s);
                break;
            case URI_HAS_Q:
                map_convert_coords_to_quadtree_string(map,x,y,zoom,location,'t',"qrts");
                s = g_strdup_printf("%s", location);
                url = replace_string(url, URI_MARKER_Q, s);
                break;
            case URI_HAS_Q0:
                map_convert_coords_to_quadtree_string(map,x,y,zoom,location,'\0', "0123");
                s = g_strdup_printf("%s", location);
                url = replace_string(url, URI_MARKER_Q0, s);
                //g_debug("FOUND " URI_MARKER_Q0);
                break;
            case URI_HAS_YS:
                //              s = g_strdup_printf("%d", y);
                //              url = replace_string(url, URI_MARKER_YS, s);
                g_warning("FOUND " URI_MARKER_YS " NOT IMPLEMENTED");
                //            retval = g_strdup_printf(repo->url,
                //                    tilex,
                //                    (1 << (MAX_ZOOM - zoom)) - tiley - 1,
                //                    zoom - (MAX_ZOOM - 17));
                break;
            case URI_HAS_R:
                s = g_strdup_printf("%d", g_random_int_range(0,4));
                url = replace_string(url, URI_MARKER_R, s);
                break;
            default:
                s = NULL;
                break;
        }

        if (s) {
            g_free(s);
            g_free(old);
        }

        i = (i << 1);

    }

    return url;
}

static void
my_log_handler (const gchar * log_domain, GLogLevelFlags log_level, const gchar * message, gpointer user_data)
{
    if (!(log_level & G_LOG_LEVEL_DEBUG) || ENABLE_DEBUG)
        g_log_default_handler (log_domain, log_level, message, user_data);
}

static float
osm_gps_map_get_scale_at_point(int zoom, float rlat, float rlon)
{
    /* world at zoom 1 == 512 pixels */
    return cos(rlat) * M_PI * OSM_EQ_RADIUS / (1<<(7+zoom));
}

static GSList *
gslist_remove_one_gobject(GSList **list, GObject *gobj)
{
    GSList *data = g_slist_find(*list, gobj);
    if (data) {
        g_object_unref(gobj);
        *list = g_slist_delete_link(*list, data);
    }
    return data;
}

static void
gslist_of_gobjects_free(GSList **list)
{
    if (list) {
        g_slist_foreach(*list, (GFunc) g_object_unref, NULL);
        g_slist_free(*list);
        *list = NULL;
    }
}

static void
gslist_of_data_free (GSList **list)
{
    if (list) {
        g_slist_foreach(*list, (GFunc) g_free, NULL);
        g_slist_free(*list);
        *list = NULL;
    }
}

static void
draw_white_rectangle(cairo_t *cr, double x, double y, double width, double height)
{
    cairo_save (cr);
    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_rectangle (cr, x, y, width, height);
    cairo_fill (cr);
    cairo_restore (cr);
}

static void
osm_gps_map_print_images (OsmGpsMap *map, cairo_t *cr)
{
    GSList *list;
    int min_x = 0,min_y = 0,max_x = 0,max_y = 0;
    int map_x0, map_y0;
    OsmGpsMapPrivate *priv = map->priv;

    map_x0 = priv->map_x - EXTRA_BORDER;
    map_y0 = priv->map_y - EXTRA_BORDER;
    for(list = priv->images; list != NULL; list = list->next)
    {
        GdkRectangle loc;
        OsmGpsMapImage *im = OSM_GPS_MAP_IMAGE(list->data);
        const OsmGpsMapPoint *pt = osm_gps_map_image_get_point(im);

        /* pixel_x,y, offsets */
        loc.x = lon2pixel(priv->map_zoom, pt->rlon) - map_x0;
        loc.y = lat2pixel(priv->map_zoom, pt->rlat) - map_y0;

        osm_gps_map_image_draw (
                         im,
                         cr,
                         &loc);

        max_x = MAX(loc.x + loc.width, max_x);
        min_x = MIN(loc.x - loc.width, min_x);
        max_y = MAX(loc.y + loc.height, max_y);
        min_y = MIN(loc.y - loc.height, min_y);
    }

    gtk_widget_queue_draw_area (
                                GTK_WIDGET(map),
                                min_x + EXTRA_BORDER, min_y + EXTRA_BORDER,
                                max_x + EXTRA_BORDER, max_y + EXTRA_BORDER);

}

static void
osm_gps_map_draw_gps_point (OsmGpsMap *map, cairo_t *cr)
{
    OsmGpsMapPrivate *priv = map->priv;
    int map_x0, map_y0;
    int x, y;
    int r, r2, mr;

    r = priv->ui_gps_point_inner_radius;
    r2 = priv->ui_gps_point_outer_radius;
    mr = MAX(3*r,r2);
    map_x0 = priv->map_x - EXTRA_BORDER;
    map_y0 = priv->map_y - EXTRA_BORDER;
    x = lon2pixel(priv->map_zoom, priv->gps->rlon) - map_x0;
    y = lat2pixel(priv->map_zoom, priv->gps->rlat) - map_y0;

    /* draw transparent area */
    if (r2 > 0) {
        cairo_set_line_width (cr, 1.5);
        cairo_set_source_rgba (cr, 0.75, 0.75, 0.75, 0.4);
        cairo_arc (cr, x, y, r2, 0, 2 * M_PI);
        cairo_fill (cr);
        /* draw transparent area border */
        cairo_set_source_rgba (cr, 0.55, 0.55, 0.55, 0.4);
        cairo_arc (cr, x, y, r2, 0, 2 * M_PI);
        cairo_stroke(cr);
    }

    /* draw ball gradient */
    if (r > 0) {
        cairo_pattern_t *pat;
        /* draw direction arrow */
        if(!isnan(priv->gps_heading)) {
            cairo_move_to (cr, x-r*cos(priv->gps_heading), y-r*sin(priv->gps_heading));
            cairo_line_to (cr, x+3*r*sin(priv->gps_heading), y-3*r*cos(priv->gps_heading));
            cairo_line_to (cr, x+r*cos(priv->gps_heading), y+r*sin(priv->gps_heading));
            cairo_close_path (cr);

            cairo_set_source_rgba (cr, 0.3, 0.3, 1.0, 0.5);
            cairo_fill_preserve (cr);

            cairo_set_line_width (cr, 1.0);
            cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.5);
            cairo_stroke(cr);
        }

        pat = cairo_pattern_create_radial (x-(r/5), y-(r/5), (r/5), x,  y, r);
        cairo_pattern_add_color_stop_rgba (pat, 0, 1, 1, 1, 1.0);
        cairo_pattern_add_color_stop_rgba (pat, 1, 0, 0, 1, 1.0);
        cairo_set_source (cr, pat);
        cairo_arc (cr, x, y, r, 0, 2 * M_PI);
        cairo_fill (cr);
        cairo_pattern_destroy (pat);
        /* draw ball border */
        cairo_set_line_width (cr, 1.0);
        cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 1.0);
        cairo_arc (cr, x, y, r, 0, 2 * M_PI);
        cairo_stroke(cr);
    }

    gtk_widget_queue_draw_area (GTK_WIDGET(map),
                                x-mr,
                                y-mr,
                                mr*2,
                                mr*2);
}

static void
osm_gps_map_blit_tile(OsmGpsMap *map, GdkPixbuf *pixbuf, cairo_t *cr, int offset_x, int offset_y,
                      int tile_zoom, int target_x, int target_y)
{
    OsmGpsMapPrivate *priv = map->priv;
    int target_zoom = priv->map_zoom;

    if (tile_zoom == target_zoom) {
        g_debug("Blit @ %d,%d", offset_x,offset_y);
        /* draw pixbuf */
        gdk_cairo_set_source_pixbuf (cr, pixbuf, offset_x, offset_y);
        cairo_paint (cr);
    } else {
        /* get an upscaled version of the pixbuf */
        GdkPixbuf *pixmap_scaled = osm_gps_map_render_tile_upscaled (
                                            map, pixbuf, tile_zoom,
                                            target_zoom, target_x, target_y);

        osm_gps_map_blit_tile (map, pixmap_scaled, cr, offset_x, offset_y,
                               target_zoom, target_x, target_y);

        g_object_unref (pixmap_scaled);
    }
}

#define MSG_RESPONSE_BODY(a)    ((a)->response_body->data)
#define MSG_RESPONSE_LEN(a)     ((a)->response_body->length)
#define MSG_RESPONSE_LEN_FORMAT "%"G_GOFFSET_FORMAT

static void
osm_gps_map_tile_download_complete (SoupSession *session, SoupMessage *msg, gpointer user_data)
{
    FILE *file;
    OsmTileDownload *dl = (OsmTileDownload *)user_data;
    OsmGpsMap *map = OSM_GPS_MAP(dl->map);
    OsmGpsMapPrivate *priv = map->priv;
    gboolean file_saved = FALSE;

    if (SOUP_STATUS_IS_SUCCESSFUL (msg->status_code)) {
        /* save tile into cachedir if one has been specified */
        if (priv->cache_dir) {
            if (g_mkdir_with_parents(dl->folder,0700) == 0) {
                file = g_fopen(dl->filename, "wb");
                if (file != NULL) {
                    fwrite (MSG_RESPONSE_BODY(msg), 1, MSG_RESPONSE_LEN(msg), file);
                    file_saved = TRUE;
                    g_debug("Wrote "MSG_RESPONSE_LEN_FORMAT" bytes to %s", MSG_RESPONSE_LEN(msg), dl->filename);
                    fclose (file);

                }
            } else {
                g_warning("Error creating tile download directory: %s", dl->folder);
            }
        }

        if (dl->redraw) {
            GdkPixbuf *pixbuf = NULL;

            /* if the file was actually stored on disk, we can simply */
            /* load and decode it from that file */
            if (priv->cache_dir) {
                if (file_saved) {
                    pixbuf = gdk_pixbuf_new_from_file (dl->filename, NULL);
                }
            } else {
                GdkPixbufLoader *loader;
                char *extension = strrchr (dl->filename, '.');

                /* parse file directly from memory */
                if (extension) {
                    loader = gdk_pixbuf_loader_new_with_type (extension+1, NULL);
                    if (!gdk_pixbuf_loader_write (loader, (unsigned char*)MSG_RESPONSE_BODY(msg), MSG_RESPONSE_LEN(msg), NULL))
                    {
                        g_warning("Error: Decoding of image failed");
                    }
                    gdk_pixbuf_loader_close(loader, NULL);

                    pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);

                    /* give up loader but keep the pixbuf */
                    g_object_ref(pixbuf);
                    g_object_unref(loader);
                } else {
                    g_warning("Error: Unable to determine image file format");
                }
            }
                
            /* Store the tile into the cache */
            if (G_LIKELY (pixbuf)) {
                OsmCachedTile *tile = g_slice_new (OsmCachedTile);
                tile->pixbuf = pixbuf;
                tile->redraw_cycle = priv->redraw_cycle;
                /* if the tile is already in the cache (it could be one
                 * rendered from another zoom level), it will be
                 * overwritten */
                g_hash_table_insert (priv->tile_cache, dl->filename, tile);
                /* NULL-ify dl->filename so that it won't be freed, as
                 * we are using it as a key in the hash table */
                dl->filename = NULL;
            }
            osm_gps_map_map_redraw_idle (map);
        }
        g_hash_table_remove(priv->tile_queue, dl->uri);
        g_object_notify(G_OBJECT(map), "tiles-queued");

        g_free(dl->folder);
        g_free(dl->filename);
        g_free(dl);
    } else {
        if ((msg->status_code == SOUP_STATUS_NOT_FOUND) || (msg->status_code == SOUP_STATUS_FORBIDDEN)) {
            g_hash_table_insert(priv->missing_tiles, dl->uri, NULL);
            g_hash_table_remove(priv->tile_queue, dl->uri);
            g_object_notify(G_OBJECT(map), "tiles-queued");
        } else if (msg->status_code == SOUP_STATUS_CANCELLED) {
            /* called as application exit or after osm_gps_map_download_cancel_all */
            g_hash_table_remove(priv->tile_queue, dl->uri);
            g_object_notify(G_OBJECT(map), "tiles-queued");
        } else {
            g_warning("Error downloading tile: %d - %s", msg->status_code, msg->reason_phrase);
            dl->ttl--;
            if (dl->ttl) {
                soup_session_requeue_message(session, msg);
                return;
            }

            g_hash_table_remove(priv->tile_queue, dl->uri);
            g_object_notify(G_OBJECT(map), "tiles-queued");
        }
    }


}

static void
osm_gps_map_download_tile (OsmGpsMap *map, int zoom, int x, int y, gboolean redraw)
{
    SoupMessage *msg;
    OsmGpsMapPrivate *priv = map->priv;
    OsmTileDownload *dl = g_new0(OsmTileDownload,1);

    // set retries
    dl->ttl = DOWNLOAD_RETRIES;

    //calculate the uri to download
    dl->uri = replace_map_uri(map, priv->repo_uri, zoom, x, y);

    //check the tile has not already been queued for download,
    //or has been attempted, and its missing
    if (g_hash_table_lookup_extended(priv->tile_queue, dl->uri, NULL, NULL) ||
        g_hash_table_lookup_extended(priv->missing_tiles, dl->uri, NULL, NULL) )
    {
        g_debug("Tile already downloading (or missing)");
        g_free(dl->uri);
        g_free(dl);
    } else {
        dl->folder = g_strdup_printf("%s%c%d%c%d%c",
                            priv->cache_dir, G_DIR_SEPARATOR,
                            zoom, G_DIR_SEPARATOR,
                            x, G_DIR_SEPARATOR);
        dl->filename = g_strdup_printf("%s%c%d%c%d%c%d.%s",
                            priv->cache_dir, G_DIR_SEPARATOR,
                            zoom, G_DIR_SEPARATOR,
                            x, G_DIR_SEPARATOR,
                            y,
                            priv->image_format);
        dl->map = map;
        dl->redraw = redraw;

        g_debug("Download tile: %d,%d z:%d\n\t%s --> %s", x, y, zoom, dl->uri, dl->filename);

        msg = soup_message_new (SOUP_METHOD_GET, dl->uri);
        if (msg) {
            if (priv->is_google) {
                //Set maps.google.com as the referrer
                g_debug("Setting Google Referrer");
                soup_message_headers_append(msg->request_headers, "Referer", "http://maps.google.com/");
                //For google satelite also set the appropriate cookie value
                if (priv->uri_format & URI_HAS_Q) {
                    const char *cookie = g_getenv("GOOGLE_COOKIE");
                    if (cookie) {
                        g_debug("Adding Google Cookie");
                        soup_message_headers_append(msg->request_headers, "Cookie", cookie);
                    }
                }
            }

            g_hash_table_insert (priv->tile_queue, dl->uri, msg);
            g_object_notify (G_OBJECT (map), "tiles-queued");
            /* the soup session unrefs the message when the download finishes */
            soup_session_queue_message (priv->soup_session, msg, osm_gps_map_tile_download_complete, dl);
        } else {
            g_warning("Could not create soup message");
            g_free(dl->uri);
            g_free(dl->folder);
            g_free(dl->filename);
            g_free(dl);
        }
    }
}

static GdkPixbuf *
osm_gps_map_load_cached_tile (OsmGpsMap *map, int zoom, int x, int y)
{
    OsmGpsMapPrivate *priv = map->priv;
    gchar *filename;
    GdkPixbuf *pixbuf = NULL;
    OsmCachedTile *tile;

    filename = g_strdup_printf("%s%c%d%c%d%c%d.%s",
                priv->cache_dir, G_DIR_SEPARATOR,
                zoom, G_DIR_SEPARATOR,
                x, G_DIR_SEPARATOR,
                y,
                priv->image_format);

    tile = g_hash_table_lookup (priv->tile_cache, filename);
    if (tile)
    {
        g_free (filename);
    }
    else
    {
        pixbuf = gdk_pixbuf_new_from_file (filename, NULL);
        if (pixbuf)
        {
            tile = g_slice_new (OsmCachedTile);
            tile->pixbuf = pixbuf;
            g_hash_table_insert (priv->tile_cache, filename, tile);
        }
        else
        {
            g_free (filename);
        }
    }

    /* set/update the redraw_cycle timestamp on the tile */
    if (tile)
    {
        tile->redraw_cycle = priv->redraw_cycle;
        pixbuf = g_object_ref (tile->pixbuf);
    }

    return pixbuf;
}

static GdkPixbuf *
osm_gps_map_find_bigger_tile (OsmGpsMap *map, int zoom, int x, int y,
                              int *zoom_found)
{
    GdkPixbuf *pixbuf;
    int next_zoom, next_x, next_y;

    if (zoom == 0) return NULL;
    next_zoom = zoom - 1;
    next_x = x / 2;
    next_y = y / 2;
    pixbuf = osm_gps_map_load_cached_tile (map, next_zoom, next_x, next_y);
    if (pixbuf)
        *zoom_found = next_zoom;
    else
        pixbuf = osm_gps_map_find_bigger_tile (map, next_zoom, next_x, next_y,
                                               zoom_found);
    return pixbuf;
}

static GdkPixbuf *
osm_gps_map_render_missing_tile_upscaled (OsmGpsMap *map, int zoom,
                                          int x, int y)
{
    GdkPixbuf *pixbuf, *big;
    int zoom_big;

    big = osm_gps_map_find_bigger_tile (map, zoom, x, y, &zoom_big);
    if (!big) return NULL;

    g_debug ("Found bigger tile (zoom = %d, wanted = %d)", zoom_big, zoom);

    pixbuf = osm_gps_map_render_tile_upscaled (map, big, zoom_big,
                                               zoom, x, y);
    g_object_unref (big);

    return pixbuf;
}
static GdkPixbuf*
osm_gps_map_render_tile_upscaled (OsmGpsMap *map, GdkPixbuf *big, int zoom_big,
                                  int zoom, int x, int y)
{
    GdkPixbuf *pixbuf, *area;
    int area_size, area_x, area_y;
    int modulo;
    int zoom_diff;

    /* get a Pixbuf for the area to magnify */
    zoom_diff = zoom - zoom_big;

    g_debug ("Upscaling by %d levels into tile %d,%d", zoom_diff, x, y);

    area_size = TILESIZE >> zoom_diff;
    modulo = 1 << zoom_diff;
    area_x = (x % modulo) * area_size;
    area_y = (y % modulo) * area_size;
    area = gdk_pixbuf_new_subpixbuf (big, area_x, area_y,
                                     area_size, area_size);
    pixbuf = gdk_pixbuf_scale_simple (area, TILESIZE, TILESIZE,
                                      GDK_INTERP_NEAREST);
    g_object_unref (area);
    return pixbuf;
}

static GdkPixbuf *
osm_gps_map_render_missing_tile (OsmGpsMap *map, int zoom, int x, int y)
{
    /* maybe TODO: render from downscaled tiles, if the following fails */
    return osm_gps_map_render_missing_tile_upscaled (map, zoom, x, y);
}

static void
osm_gps_map_load_tile (OsmGpsMap *map, cairo_t *cr, int zoom, int x, int y, int offset_x, int offset_y)
{
    OsmGpsMapPrivate *priv = map->priv;
    gchar *filename;
    GdkPixbuf *pixbuf;
    int zoom_offset = priv->tile_zoom_offset;
    int target_x, target_y;

    g_debug("Load virtual tile %d,%d (%d,%d) z:%d", x, y, offset_x, offset_y, zoom);

    if (zoom > MIN_ZOOM) {
      zoom -= zoom_offset;
      x >>= zoom_offset;
      y >>= zoom_offset;
    }

    target_x = x;
    target_y = y;

    g_debug("Load actual tile %d,%d (%d,%d) z:%d", x, y, offset_x, offset_y, zoom);

    if (priv->map_source == OSM_GPS_MAP_SOURCE_NULL) {
        osm_gps_map_blit_tile(map, priv->null_tile, cr, offset_x, offset_y,
                              priv->map_zoom, target_x, target_y);
        return;
    }

    filename = g_strdup_printf("%s%c%d%c%d%c%d.%s",
                priv->cache_dir, G_DIR_SEPARATOR,
                zoom, G_DIR_SEPARATOR,
                x, G_DIR_SEPARATOR,
                y,
                priv->image_format);

    /* try to get file from internal cache first */
    if(!(pixbuf = osm_gps_map_load_cached_tile(map, zoom, x, y)))
        pixbuf = gdk_pixbuf_new_from_file (filename, NULL);

    if(pixbuf) {
        g_debug("Found tile %s", filename);
        osm_gps_map_blit_tile(map, pixbuf, cr, offset_x, offset_y,
                              zoom, target_x, target_y);
        g_object_unref (pixbuf);
    } else {
        if (priv->map_auto_download_enabled) {
            osm_gps_map_download_tile(map, zoom, x, y, TRUE);
        }

        /* try to render the tile by scaling cached tiles from other zoom
         * levels */
        pixbuf = osm_gps_map_render_missing_tile (map, zoom, x, y);
        if (pixbuf) {
            osm_gps_map_blit_tile(map, pixbuf, cr, offset_x, offset_y,
                                   zoom, target_x, target_y);
            g_object_unref (pixbuf);
        } else {
            /* prevent some artifacts when drawing not yet loaded areas. */
            g_warning ("Error getting missing tile"); /* FIXME: is this a warning? */
            draw_white_rectangle (cr, offset_x, offset_y, TILESIZE, TILESIZE);
        }
    }
    g_free(filename);
}

static void
osm_gps_map_fill_tiles_pixel (OsmGpsMap *map, cairo_t *cr)
{
    OsmGpsMapPrivate *priv = map->priv;
    GtkAllocation allocation;
    int i,j, tile_x0, tile_y0, tiles_nx, tiles_ny;
    int offset_xn = 0;
    int offset_yn = 0;
    int offset_x;
    int offset_y;

    g_debug("Fill tiles: %d,%d z:%d", priv->map_x, priv->map_y, priv->map_zoom);

    gtk_widget_get_allocation(GTK_WIDGET(map), &allocation);

    offset_x = - priv->map_x % TILESIZE;
    offset_y = - priv->map_y % TILESIZE;
    if (offset_x > 0) offset_x -= TILESIZE;
    if (offset_y > 0) offset_y -= TILESIZE;

    offset_xn = offset_x + EXTRA_BORDER;
    offset_yn = offset_y + EXTRA_BORDER;

    tiles_nx = (allocation.width  - offset_x) / TILESIZE + 1;
    tiles_ny = (allocation.height - offset_y) / TILESIZE + 1;

    tile_x0 =  floor((float)priv->map_x / (float)TILESIZE);
    tile_y0 =  floor((float)priv->map_y / (float)TILESIZE);

    for (i=tile_x0; i<(tile_x0+tiles_nx);i++)
    {
        for (j=tile_y0;  j<(tile_y0+tiles_ny); j++)
        {
            if( j<0 || i<0 || i>=exp(priv->map_zoom * M_LN2) || j>=exp(priv->map_zoom * M_LN2))
            {
                /* draw white in areas outside map (i.e. when zoomed right out) */
                draw_white_rectangle (cr, offset_xn, offset_yn, TILESIZE, TILESIZE);
            }
            else
            {
                osm_gps_map_load_tile(map,
                                      cr,
                                      priv->map_zoom,
                                      i,j,
                                      offset_xn - EXTRA_BORDER,offset_yn - EXTRA_BORDER);
            }
            offset_yn += TILESIZE;
        }
        offset_xn += TILESIZE;
        offset_yn = offset_y + EXTRA_BORDER;
    }
}

static void
osm_gps_map_print_track (OsmGpsMap *map, OsmGpsMapTrack *track, cairo_t *cr)
{
    OsmGpsMapPrivate *priv = map->priv;

    GSList *pt,*points;
    int x,y;
    int min_x = 0,min_y = 0,max_x = 0,max_y = 0;
    gfloat lw, alpha;
    int map_x0, map_y0;
    GdkRGBA color;

    g_object_get (track,
            "track", &points,
            "line-width", &lw,
            "alpha", &alpha,
            NULL);
    osm_gps_map_track_get_color(track, &color);

    if (points == NULL)
        return;

    cairo_set_line_width (cr, lw);
    cairo_set_source_rgba (cr, color.red/65535.0, color.green/65535.0, color.blue/65535.0, alpha);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);

    map_x0 = priv->map_x - EXTRA_BORDER;
    map_y0 = priv->map_y - EXTRA_BORDER;
    for(pt = points; pt != NULL; pt = pt->next)
    {
        OsmGpsMapPoint *tp = pt->data;

        x = lon2pixel(priv->map_zoom, tp->rlon) - map_x0;
        y = lat2pixel(priv->map_zoom, tp->rlat) - map_y0;

        /* first time through loop */
        if (pt == points) {
            cairo_move_to(cr, x, y);
        }

        cairo_line_to(cr, x, y);

        max_x = MAX(x,max_x);
        min_x = MIN(x,min_x);
        max_y = MAX(y,max_y);
        min_y = MIN(y,min_y);
    }

    gtk_widget_queue_draw_area (
                                GTK_WIDGET(map),
                                min_x - lw,
                                min_y - lw,
                                max_x + (lw * 2),
                                max_y + (lw * 2));

    cairo_stroke(cr);
}

/* Prints the gps trip history, and any other tracks */
static void
osm_gps_map_print_tracks (OsmGpsMap *map, cairo_t *cr)
{
    GSList *tmp;
    OsmGpsMapPrivate *priv = map->priv;

    if (priv->trip_history_show_enabled) {
        osm_gps_map_print_track (map, priv->gps_track, cr);
    }

    if (priv->tracks) {
        tmp = priv->tracks;
        while (tmp != NULL) {
            osm_gps_map_print_track (map, OSM_GPS_MAP_TRACK(tmp->data), cr);
            tmp = g_slist_next(tmp);
        }
    }
}

static gboolean
osm_gps_map_purge_cache_check(gpointer key, gpointer value, gpointer user)
{
   return (((OsmCachedTile*)value)->redraw_cycle != ((OsmGpsMapPrivate*)user)->redraw_cycle);
}

static void
osm_gps_map_purge_cache (OsmGpsMap *map)
{
   OsmGpsMapPrivate *priv = map->priv;

   if (g_hash_table_size (priv->tile_cache) < priv->max_tile_cache_size)
       return;

   /* run through the cache, and remove the tiles which have not been used
    * during the last redraw operation */
   g_hash_table_foreach_remove(priv->tile_cache, osm_gps_map_purge_cache_check, priv);
}

static gboolean
osm_gps_map_map_redraw (OsmGpsMap *map)
{
    cairo_t *cr;
    int w, h;
    OsmGpsMapPrivate *priv = map->priv;
    GtkWidget *widget = GTK_WIDGET(map);

    priv->idle_map_redraw = 0;

    /* dont't redraw if we have not been shown yet */
    if (!priv->pixmap)
        return FALSE;

    /* don't redraw the entire map while the OSD is doing */
    /* some animation or the like. This is to keep the animation */
    /* fluid */
    if (priv->layers) {
        GSList *list;
        for(list = priv->layers; list != NULL; list = list->next) {
            OsmGpsMapLayer *layer = list->data;
            if (osm_gps_map_layer_busy(layer))
                return FALSE;
        }
    }

    /* the motion_notify handler uses priv->surface to redraw the area; if we
     * change it while we are dragging, we will end up showing it in the wrong
     * place. This could be fixed by carefully recompute the coordinates, but
     * for now it's easier just to disable redrawing the map while dragging */
    if (priv->is_dragging)
        return FALSE;

    /* paint to the backing surface */
    cr = cairo_create (priv->pixmap);

    /* undo all offsets that may have happened when dragging */
    priv->drag_mouse_dx = 0;
    priv->drag_mouse_dy = 0;

    priv->redraw_cycle++;

    /* clear white background */
    w = gtk_widget_get_allocated_width (widget);
    h = gtk_widget_get_allocated_width (widget);
    draw_white_rectangle(cr, 0, 0, w + EXTRA_BORDER * 2, h + EXTRA_BORDER * 2);

    osm_gps_map_fill_tiles_pixel(map, cr);

    osm_gps_map_print_tracks(map, cr);
    osm_gps_map_print_images(map, cr);

    /* draw the gps point using the appropriate virtual private method */
    if (priv->gps_track_used && priv->gps_point_enabled) {
        OsmGpsMapClass *klass = OSM_GPS_MAP_GET_CLASS(map);
        if (klass->draw_gps_point)
            klass->draw_gps_point (map, cr);
    }

    if (priv->layers) {
        GSList *list;
        for(list = priv->layers; list != NULL; list = list->next) {
            OsmGpsMapLayer *layer = list->data;
            osm_gps_map_layer_render (layer, map);
        }
    }

    osm_gps_map_purge_cache(map);
    gtk_widget_queue_draw (GTK_WIDGET (map));

    cairo_destroy (cr);

    return FALSE;
}

static void
osm_gps_map_map_redraw_idle (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;

    if (priv->idle_map_redraw == 0)
        priv->idle_map_redraw = g_idle_add ((GSourceFunc)osm_gps_map_map_redraw, map);
}

/* call this to update center_rlat and center_rlon after
 * changin map_x or map_y */
static void
center_coord_update(OsmGpsMap *map) {

    GtkWidget *widget = GTK_WIDGET(map);
    OsmGpsMapPrivate *priv = map->priv;
    GtkAllocation allocation;

    gtk_widget_get_allocation(widget, &allocation);
    gint pixel_x = priv->map_x + allocation.width/2;
    gint pixel_y = priv->map_y + allocation.height/2;

    priv->center_rlon = pixel2lon(priv->map_zoom, pixel_x);
    priv->center_rlat = pixel2lat(priv->map_zoom, pixel_y);

    g_signal_emit_by_name(widget, "changed");
}

/* Automatically center the map if the current point, i.e the most recent
 * gps point, approaches the edge, and map_auto_center is set. Does not
 * request the map be redrawn */
static void
maybe_autocenter_map (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv;
    GtkAllocation allocation;

    g_return_if_fail (OSM_IS_GPS_MAP (map));
    priv = map->priv;
    gtk_widget_get_allocation(GTK_WIDGET(map), &allocation);

    if(priv->map_auto_center_enabled)   {
        int pixel_x = lon2pixel(priv->map_zoom, priv->gps->rlon);
        int pixel_y = lat2pixel(priv->map_zoom, priv->gps->rlat);
        int x = pixel_x - priv->map_x;
        int y = pixel_y - priv->map_y;
        int width = allocation.width;
        int height = allocation.height;
        if( x < (width/2 - width/8)     || x > (width/2 + width/8)  ||
            y < (height/2 - height/8)   || y > (height/2 + height/8)) {

            priv->map_x = pixel_x - allocation.width/2;
            priv->map_y = pixel_y - allocation.height/2;
            center_coord_update(map);
        }
    }
}

static gboolean 
on_window_key_press(GtkWidget *widget, GdkEventKey *event, OsmGpsMapPrivate *priv) 
{
    int i;
    int step;
    gboolean handled;
    GtkAllocation allocation;
    OsmGpsMap *map = OSM_GPS_MAP(widget);

    /* if no keybindings are set, let the app handle them... */
    if (!priv->keybindings_enabled)
        return FALSE;

    handled = FALSE;
    gtk_widget_get_allocation(GTK_WIDGET(map), &allocation);
    step = allocation.width/OSM_GPS_MAP_SCROLL_STEP;

    /* the map handles some keys on its own */
    for (i = 0; i < OSM_GPS_MAP_KEY_MAX; i++) {
        /* not the key we have a binding for */
        if (map->priv->keybindings[i] != event->keyval)
            continue;

        switch(i) {
            case OSM_GPS_MAP_KEY_FULLSCREEN: {
                GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(widget));
                if(!priv->is_fullscreen)
                    gtk_window_fullscreen(GTK_WINDOW(toplevel));
                else
                    gtk_window_unfullscreen(GTK_WINDOW(toplevel));

                priv->is_fullscreen = !priv->is_fullscreen;
                handled = TRUE;
                } break;
            case OSM_GPS_MAP_KEY_ZOOMIN:
                osm_gps_map_zoom_in(map);
                handled = TRUE;
                break;
            case OSM_GPS_MAP_KEY_ZOOMOUT:
                osm_gps_map_zoom_out(map);
                handled = TRUE;
                break;
            case OSM_GPS_MAP_KEY_UP:
                priv->map_y -= step;
                center_coord_update(map);
                osm_gps_map_map_redraw_idle(map);
                handled = TRUE;
                break;
            case OSM_GPS_MAP_KEY_DOWN:
                priv->map_y += step;
                center_coord_update(map);
                osm_gps_map_map_redraw_idle(map);
                handled = TRUE;
                break;
              case OSM_GPS_MAP_KEY_LEFT:
                priv->map_x -= step;
                center_coord_update(map);
                osm_gps_map_map_redraw_idle(map);
                handled = TRUE;
                break;
            case OSM_GPS_MAP_KEY_RIGHT:
                priv->map_x += step;
                center_coord_update(map);
                osm_gps_map_map_redraw_idle(map);
                handled = TRUE;
                break;
            default:
                break;
        }
    }

    return handled;
}

static void
on_gps_point_added (OsmGpsMapTrack *track, OsmGpsMapPoint *point, OsmGpsMap *map)
{
    osm_gps_map_map_redraw_idle (map);
    maybe_autocenter_map (map);
}

static void
on_track_changed (OsmGpsMapTrack *track, GParamSpec *pspec, OsmGpsMap *map)
{
    osm_gps_map_map_redraw_idle (map);
}

static void
osm_gps_map_init (OsmGpsMap *object)
{
    int i;
    OsmGpsMapPrivate *priv;

    priv = G_TYPE_INSTANCE_GET_PRIVATE (object, OSM_TYPE_GPS_MAP, OsmGpsMapPrivate);
    object->priv = priv;

    priv->pixmap = NULL;

    priv->trip_history = NULL;
    priv->gps = osm_gps_map_point_new_radians(0.0, 0.0);
    priv->gps_track_used = FALSE;
    priv->gps_heading = OSM_GPS_MAP_INVALID;

    priv->gps_track = osm_gps_map_track_new();
    g_signal_connect(priv->gps_track, "point-added",
                    G_CALLBACK(on_gps_point_added), object);
    g_signal_connect(priv->gps_track, "notify",
                    G_CALLBACK(on_track_changed), object);

    priv->tracks = NULL;
    priv->images = NULL;
    priv->layers = NULL;

    priv->drag_counter = 0;
    priv->drag_mouse_dx = 0;
    priv->drag_mouse_dy = 0;
    priv->drag_start_mouse_x = 0;
    priv->drag_start_mouse_y = 0;

    priv->uri_format = 0;
    priv->is_google = FALSE;

    priv->map_source = -1;

    priv->keybindings_enabled = FALSE;
    for (i = 0; i < OSM_GPS_MAP_KEY_MAX; i++)
        priv->keybindings[i] = 0;


    /* set the user agent */
    priv->soup_session =
        soup_session_async_new_with_options(SOUP_SESSION_USER_AGENT,
                                            USER_AGENT, NULL);

    /* Hash table which maps tile d/l URIs to SoupMessage requests, the hashtable
       must free the key, the soup session unrefs the message */
    priv->tile_queue = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free, NULL);

    //Some mapping providers (Google) have varying degrees of tiles at multiple
    //zoom levels
    priv->missing_tiles = g_hash_table_new (g_str_hash, g_str_equal);

    /* memory cache for most recently used tiles */
    priv->tile_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free, (GDestroyNotify)cached_tile_free);
    priv->max_tile_cache_size = 20;

    gtk_widget_add_events (GTK_WIDGET (object),
                           GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                           GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK |
                           GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
#ifdef HAVE_GDK_EVENT_GET_SCROLL_DELTAS
    gtk_widget_add_events (GTK_WIDGET (object), GDK_SMOOTH_SCROLL_MASK)
#endif
    gtk_widget_set_can_focus (GTK_WIDGET (object), TRUE);

    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MASK, my_log_handler, NULL);

    /* setup signal handlers */
    g_signal_connect(object, "key_press_event",
                    G_CALLBACK(on_window_key_press), priv);
}

static char*
osm_gps_map_get_cache_base_dir(OsmGpsMapPrivate *priv)
{
    if (priv->tile_base_dir)
        return g_strdup(priv->tile_base_dir);
    return osm_gps_map_get_default_cache_directory();
}

static void
osm_gps_map_setup(OsmGpsMap *map)
{
    const char *uri;
    OsmGpsMapPrivate *priv = map->priv;

   /* user can specify a map source ID, or a repo URI as the map source */
    uri = osm_gps_map_source_get_repo_uri(OSM_GPS_MAP_SOURCE_NULL);
    if ( (priv->map_source == 0) || (strcmp(priv->repo_uri, uri) == 0) ) {
        g_debug("Using null source");
        priv->map_source = OSM_GPS_MAP_SOURCE_NULL;

        priv->null_tile = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 256, 256);
        gdk_pixbuf_fill(priv->null_tile, 0xcccccc00);
    }
    else if (priv->map_source >= 0) {
        /* check if the source given is valid */
        uri = osm_gps_map_source_get_repo_uri(priv->map_source);
        if (uri) {
            g_debug("Setting map source from ID");
            g_free(priv->repo_uri);

            priv->repo_uri = g_strdup(uri);
            priv->image_format = g_strdup(
                osm_gps_map_source_get_image_format(priv->map_source));
            priv->max_zoom = osm_gps_map_source_get_max_zoom(priv->map_source);
            priv->min_zoom = osm_gps_map_source_get_min_zoom(priv->map_source);
        }
    }
    /* parse the source uri */
    inspect_map_uri(priv);

    /* setup the tile cache */
    if ( g_strcmp0(priv->tile_dir, OSM_GPS_MAP_CACHE_DISABLED) == 0 ) {
        priv->cache_dir = NULL;
    } else if ( g_strcmp0(priv->tile_dir, OSM_GPS_MAP_CACHE_AUTO) == 0 ) {
        char *base = osm_gps_map_get_cache_base_dir(priv);
        char *md5 = g_compute_checksum_for_string (G_CHECKSUM_MD5, priv->repo_uri, -1);
        priv->cache_dir = g_strdup_printf("%s%c%s", base, G_DIR_SEPARATOR, md5);
        g_free(base);
        g_free(md5);
    } else if ( g_strcmp0(priv->tile_dir, OSM_GPS_MAP_CACHE_FRIENDLY) == 0 ) {
        char *base = osm_gps_map_get_cache_base_dir(priv);
        const char *fname = osm_gps_map_source_get_friendly_name(priv->map_source);
        priv->cache_dir = g_strdup_printf("%s%c%s", base, G_DIR_SEPARATOR, fname);
        g_free(base);
    } else {
        /* the simple case is handled in g_object_set(PROP_TILE_CACHE_DIR) */
    }
    g_debug("Cache dir: %s", priv->cache_dir);

    /* check if we are being called for a second (or more) time in the lifetime
       of the object, and if so, do some extra cleanup */
    if ( priv->is_constructed ) {
        g_debug("Setup called again in map lifetime");
        /* flush the ram cache */
        g_hash_table_remove_all(priv->tile_cache);

        /* adjust zoom if necessary */
        if(priv->map_zoom > priv->max_zoom)
            osm_gps_map_set_zoom(map, priv->max_zoom);

        if(priv->map_zoom < priv->min_zoom)
            osm_gps_map_set_zoom(map, priv->min_zoom);

        osm_gps_map_map_redraw_idle(map);
    }
}

static GObject *
osm_gps_map_constructor (GType gtype, guint n_properties, GObjectConstructParam *properties)
{
    GObject *object;
    OsmGpsMap *map;

    /* always chain up to the parent constructor */
    object = G_OBJECT_CLASS(osm_gps_map_parent_class)->constructor(gtype, n_properties, properties);

    map = OSM_GPS_MAP(object);

    osm_gps_map_setup(map);
    map->priv->is_constructed = TRUE;

    return object;
}

static void
osm_gps_map_dispose (GObject *object)
{
    OsmGpsMap *map = OSM_GPS_MAP(object);
    OsmGpsMapPrivate *priv = map->priv;

    if (priv->is_disposed)
        return;

    priv->is_disposed = TRUE;

    soup_session_abort(priv->soup_session);
    g_object_unref(priv->soup_session);

    g_object_unref(priv->gps_track);

    g_hash_table_destroy(priv->tile_queue);
    g_hash_table_destroy(priv->missing_tiles);
    g_hash_table_destroy(priv->tile_cache);

    /* images and layers contain GObjects which need unreffing, so free here */
    gslist_of_gobjects_free(&priv->images);
    gslist_of_gobjects_free(&priv->layers);
    gslist_of_gobjects_free(&priv->tracks);

    if(priv->pixmap)
        cairo_surface_destroy (priv->pixmap);

    if (priv->null_tile)
        g_object_unref (priv->null_tile);

    if (priv->idle_map_redraw != 0)
        g_source_remove (priv->idle_map_redraw);

    if (priv->drag_expose_source != 0)
        g_source_remove (priv->drag_expose_source);

    g_free(priv->gps);


    G_OBJECT_CLASS (osm_gps_map_parent_class)->dispose (object);
}

static void
osm_gps_map_finalize (GObject *object)
{
    OsmGpsMap *map = OSM_GPS_MAP(object);
    OsmGpsMapPrivate *priv = map->priv;

    if (priv->tile_dir)
        g_free(priv->tile_dir);

    if (priv->cache_dir)
        g_free(priv->cache_dir);

    g_free(priv->repo_uri);
    g_free(priv->image_format);

    /* trip and tracks contain simple non GObject types, so free them here */
    gslist_of_data_free(&priv->trip_history);

    G_OBJECT_CLASS (osm_gps_map_parent_class)->finalize (object);
}

static void
osm_gps_map_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    g_return_if_fail (OSM_IS_GPS_MAP (object));
    OsmGpsMap *map = OSM_GPS_MAP(object);
    OsmGpsMapPrivate *priv = map->priv;

    switch (prop_id)
    {
        case PROP_AUTO_CENTER:
            priv->map_auto_center_enabled = g_value_get_boolean (value);
            break;
        case PROP_RECORD_TRIP_HISTORY:
            priv->trip_history_record_enabled = g_value_get_boolean (value);
            break;
        case PROP_SHOW_TRIP_HISTORY:
            priv->trip_history_show_enabled = g_value_get_boolean (value);
            break;
        case PROP_AUTO_DOWNLOAD:
            priv->map_auto_download_enabled = g_value_get_boolean (value);
            break;
        case PROP_REPO_URI:
            priv->repo_uri = g_value_dup_string (value);
            break;
        case PROP_PROXY_URI:
            if ( g_value_get_string(value) ) {
                priv->proxy_uri = g_value_dup_string (value);
                g_debug("Setting proxy server: %s", priv->proxy_uri);

                GValue val = {0};
                SoupURI* uri = soup_uri_new(priv->proxy_uri);
                g_value_init(&val, SOUP_TYPE_URI);
                g_value_take_boxed(&val, uri);
                g_object_set_property(G_OBJECT(priv->soup_session),SOUP_SESSION_PROXY_URI,&val);

            } else {
                priv->proxy_uri = NULL;
            }
            break;
        case PROP_TILE_CACHE_DIR:
            if ( g_value_get_string(value) ) {
                priv->tile_dir = g_value_dup_string (value);
                if ((g_strcmp0(priv->tile_dir, OSM_GPS_MAP_CACHE_DISABLED) == 0)    ||
                    (g_strcmp0(priv->tile_dir, OSM_GPS_MAP_CACHE_AUTO) == 0)        ||
                    (g_strcmp0(priv->tile_dir, OSM_GPS_MAP_CACHE_FRIENDLY) == 0)) {
                    /* this case is handled by osm_gps_map_setup */
                } else {
                    priv->cache_dir = g_strdup(priv->tile_dir);
                    g_debug("Cache dir: %s", priv->cache_dir);
                }
            } else {
                priv->tile_dir = g_strdup(OSM_GPS_MAP_CACHE_DISABLED);
            }
            break;
        case PROP_TILE_CACHE_BASE_DIR:
            priv->tile_base_dir = g_value_dup_string (value);
            break;
        case PROP_TILE_ZOOM_OFFSET:
            priv->tile_zoom_offset = g_value_get_int (value);
            break;
        case PROP_ZOOM:
            priv->map_zoom = g_value_get_int (value);
            break;
        case PROP_MAX_ZOOM:
            priv->max_zoom = g_value_get_int (value);
            break;
        case PROP_MIN_ZOOM:
            priv->min_zoom = g_value_get_int (value);
            break;
        case PROP_MAP_X:
            priv->map_x = g_value_get_int (value);
            center_coord_update(map);
            break;
        case PROP_MAP_Y:
            priv->map_y = g_value_get_int (value);
            center_coord_update(map);
            break;
        case PROP_GPS_TRACK_WIDTH:
            g_object_set (priv->gps_track,
                    "line-width", g_value_get_float (value),
                    NULL);
            break;
        case PROP_GPS_POINT_R1:
            priv->ui_gps_point_inner_radius = g_value_get_int (value);
            break;
        case PROP_GPS_POINT_R2:
            priv->ui_gps_point_outer_radius = g_value_get_int (value);
            break;
        case PROP_MAP_SOURCE: {
            gint old = priv->map_source;
            priv->map_source = g_value_get_int (value);
            if(old >= OSM_GPS_MAP_SOURCE_NULL && 
               priv->map_source != old &&
               priv->map_source >= OSM_GPS_MAP_SOURCE_NULL &&
               priv->map_source <= OSM_GPS_MAP_SOURCE_LAST) {

                if (!priv->is_constructed)
                    g_critical("Map source setup called twice");

                /* we now have to switch the entire map */
                osm_gps_map_setup(map);

            } } break;
        case PROP_IMAGE_FORMAT:
            priv->image_format = g_value_dup_string (value);
            break;
        case PROP_DRAG_LIMIT:
            priv->drag_limit = g_value_get_int (value);
            break;
        case PROP_AUTO_CENTER_THRESHOLD:
            priv->map_auto_center_threshold = g_value_get_float (value);
            break;
        case PROP_SHOW_GPS_POINT:
            priv->gps_point_enabled = g_value_get_boolean (value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static void
osm_gps_map_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    g_return_if_fail (OSM_IS_GPS_MAP (object));
    OsmGpsMap *map = OSM_GPS_MAP(object);
    OsmGpsMapPrivate *priv = map->priv;

    switch (prop_id)
    {
        case PROP_AUTO_CENTER:
            g_value_set_boolean(value, priv->map_auto_center_enabled);
            break;
        case PROP_RECORD_TRIP_HISTORY:
            g_value_set_boolean(value, priv->trip_history_record_enabled);
            break;
        case PROP_SHOW_TRIP_HISTORY:
            g_value_set_boolean(value, priv->trip_history_show_enabled);
            break;
        case PROP_AUTO_DOWNLOAD:
            g_value_set_boolean(value, priv->map_auto_download_enabled);
            break;
        case PROP_REPO_URI:
            g_value_set_string(value, priv->repo_uri);
            break;
        case PROP_PROXY_URI:
            g_value_set_string(value, priv->proxy_uri);
            break;
        case PROP_TILE_CACHE_DIR:
            g_value_set_string(value, priv->cache_dir);
            break;
        case PROP_TILE_CACHE_BASE_DIR:
            g_value_set_string(value, priv->tile_base_dir);
            break;
        case PROP_TILE_ZOOM_OFFSET:
            g_value_set_int(value, priv->tile_zoom_offset);
            break;
        case PROP_ZOOM:
            g_value_set_int(value, priv->map_zoom);
            break;
        case PROP_MAX_ZOOM:
            g_value_set_int(value, priv->max_zoom);
            break;
        case PROP_MIN_ZOOM:
            g_value_set_int(value, priv->min_zoom);
            break;
        case PROP_LATITUDE:
            g_value_set_float(value, rad2deg(priv->center_rlat));
            break;
        case PROP_LONGITUDE:
            g_value_set_float(value, rad2deg(priv->center_rlon));
            break;
        case PROP_MAP_X:
            g_value_set_int(value, priv->map_x);
            break;
        case PROP_MAP_Y:
            g_value_set_int(value, priv->map_y);
            break;
        case PROP_TILES_QUEUED:
            g_value_set_int(value, g_hash_table_size(priv->tile_queue));
            break;
        case PROP_GPS_TRACK_WIDTH: {
            gfloat f;
            g_object_get (priv->gps_track, "line-width", &f, NULL);
            g_value_set_float (value, f);
            } break;
        case PROP_GPS_POINT_R1:
            g_value_set_int(value, priv->ui_gps_point_inner_radius);
            break;
        case PROP_GPS_POINT_R2:
            g_value_set_int(value, priv->ui_gps_point_outer_radius);
            break;
        case PROP_MAP_SOURCE:
            g_value_set_int(value, priv->map_source);
            break;
        case PROP_IMAGE_FORMAT:
            g_value_set_string(value, priv->image_format);
            break;
        case PROP_DRAG_LIMIT:
            g_value_set_int(value, priv->drag_limit);
            break;
        case PROP_AUTO_CENTER_THRESHOLD:
            g_value_set_float(value, priv->map_auto_center_threshold);
            break;
        case PROP_SHOW_GPS_POINT:
            g_value_set_boolean(value, priv->gps_point_enabled);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static gboolean
osm_gps_map_scroll_event (GtkWidget *widget, GdkEventScroll  *event)
{
    OsmGpsMap *map;
    OsmGpsMapPoint *pt;
    float lat, lon;

    map = OSM_GPS_MAP(widget);
    pt = osm_gps_map_point_new_degrees(0.0,0.0);
    /* arguably we could use get_event_location here, but I'm not convinced it
    is forward compatible to cast between GdkEventScroll and GtkEventButton */
    osm_gps_map_convert_screen_to_geographic(map, event->x, event->y, pt);
    osm_gps_map_point_get_degrees (pt, &lat, &lon);

    if (event->direction == GDK_SCROLL_UP)
        osm_gps_map_set_center_and_zoom(map, lat, lon, map->priv->map_zoom+1);
    else if (event->direction == GDK_SCROLL_DOWN)
        osm_gps_map_set_center_and_zoom(map, lat, lon, map->priv->map_zoom-1);

    osm_gps_map_point_free (pt);

    return FALSE;
}

static gboolean
osm_gps_map_button_press (GtkWidget *widget, GdkEventButton *event)
{
    OsmGpsMap *map = OSM_GPS_MAP(widget);
    OsmGpsMapPrivate *priv = map->priv;

    if (priv->layers) {
        GSList *list;
        for(list = priv->layers; list != NULL; list = list->next) {
            OsmGpsMapLayer *layer = list->data;
            if (osm_gps_map_layer_button_press(layer, map, event))
                return FALSE;
        }
    }

    priv->is_button_down = TRUE;
    priv->drag_counter = 0;
    priv->drag_start_mouse_x = (int) event->x;
    priv->drag_start_mouse_y = (int) event->y;
    priv->drag_start_map_x = priv->map_x;
    priv->drag_start_map_y = priv->map_y;

    return FALSE;
}

static gboolean
osm_gps_map_button_release (GtkWidget *widget, GdkEventButton *event)
{
    OsmGpsMap *map = OSM_GPS_MAP(widget);
    OsmGpsMapPrivate *priv = map->priv;

    if(!priv->is_button_down)
        return FALSE;

    if (priv->is_dragging)
    {
        priv->is_dragging = FALSE;

        priv->map_x = priv->drag_start_map_x;
        priv->map_y = priv->drag_start_map_y;

        priv->map_x += (priv->drag_start_mouse_x - (int) event->x);
        priv->map_y += (priv->drag_start_mouse_y - (int) event->y);

        center_coord_update(map);

        osm_gps_map_map_redraw_idle(map);
    }

    priv->drag_counter = -1;
    priv->is_button_down = FALSE;

    return FALSE;
}

static gboolean
osm_gps_map_idle_expose (GtkWidget *widget)
{
    OsmGpsMapPrivate *priv = OSM_GPS_MAP(widget)->priv;
    priv->drag_expose_source = 0;
    gtk_widget_queue_draw (widget);
    return FALSE;
}

static gboolean
osm_gps_map_motion_notify (GtkWidget *widget, GdkEventMotion  *event)
{
    GdkModifierType state;
    OsmGpsMap *map = OSM_GPS_MAP(widget);
    OsmGpsMapPrivate *priv = map->priv;
    gint x, y;

    GdkDeviceManager* manager = gdk_display_get_device_manager( gdk_display_get_default() );
    GdkDevice* pointer = gdk_device_manager_get_client_pointer( manager);

    if(!priv->is_button_down)
        return FALSE;

    if (event->is_hint)
        // gdk_window_get_pointer (event->window, &x, &y, &state);
        gdk_window_get_device_position( event->window, pointer, &x, &y, &state);

    else
    {
        x = event->x;
        y = event->y;
        state = event->state;
    }

    // are we being dragged
    if (!(state & GDK_BUTTON1_MASK))
        return FALSE;

    if (priv->drag_counter < 0) 
        return FALSE;

    /* not yet dragged far enough? */
    if(!priv->drag_counter &&
       ( (x - priv->drag_start_mouse_x) * (x - priv->drag_start_mouse_x) + 
         (y - priv->drag_start_mouse_y) * (y - priv->drag_start_mouse_y) <
         priv->drag_limit*priv->drag_limit))
        return FALSE;

    priv->drag_counter++;

    priv->is_dragging = TRUE;

    if (priv->map_auto_center_enabled)
        g_object_set(G_OBJECT(widget), "auto-center", FALSE, NULL);

    priv->drag_mouse_dx = x - priv->drag_start_mouse_x;
    priv->drag_mouse_dy = y - priv->drag_start_mouse_y;

    /* instead of redrawing directly just add an idle function */
    if (!priv->drag_expose_source)
        priv->drag_expose_source = 
            g_idle_add ((GSourceFunc)osm_gps_map_idle_expose, widget);

    return FALSE;
}

static gboolean
osm_gps_map_configure (GtkWidget *widget, GdkEventConfigure *event)
{
    int w,h;
    GdkWindow *window;
    OsmGpsMap *map = OSM_GPS_MAP(widget);
    OsmGpsMapPrivate *priv = map->priv;

    if (priv->pixmap)
        cairo_surface_destroy (priv->pixmap);

    w = gtk_widget_get_allocated_width (widget);
    h = gtk_widget_get_allocated_height (widget);
    window = gtk_widget_get_window(widget);

    priv->pixmap = gdk_window_create_similar_surface (
                        window,
                        CAIRO_CONTENT_COLOR,
                        w + EXTRA_BORDER * 2,
                        h + EXTRA_BORDER * 2);

    // pixel_x,y, offsets
    gint pixel_x = lon2pixel(priv->map_zoom, priv->center_rlon);
    gint pixel_y = lat2pixel(priv->map_zoom, priv->center_rlat);

    priv->map_x = pixel_x - w/2;
    priv->map_y = pixel_y - w/2;

    osm_gps_map_map_redraw(OSM_GPS_MAP(widget));

    g_signal_emit_by_name(widget, "changed");

    return FALSE;
}

static gboolean
osm_gps_map_draw (GtkWidget *widget, cairo_t *cr)
{
    OsmGpsMap *map = OSM_GPS_MAP(widget);
    OsmGpsMapPrivate *priv = map->priv;

    if (!priv->drag_mouse_dx && !priv->drag_mouse_dy) {
        cairo_set_source_surface (cr, priv->pixmap, 0, 0);
    } else {
        cairo_set_source_surface (cr, priv->pixmap,
            priv->drag_mouse_dx - EXTRA_BORDER,
            priv->drag_mouse_dy - EXTRA_BORDER);
    }

    cairo_paint (cr);

    if (priv->layers) {
        GSList *list;
        for(list = priv->layers; list != NULL; list = list->next) {
            OsmGpsMapLayer *layer = list->data;
            osm_gps_map_layer_draw(layer, map, cr);
        }
    }

    return FALSE;
}

static void
osm_gps_map_class_init (OsmGpsMapClass *klass)
{
    GObjectClass* object_class;
    GtkWidgetClass *widget_class;

    g_type_class_add_private (klass, sizeof (OsmGpsMapPrivate));

    object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = osm_gps_map_dispose;
    object_class->finalize = osm_gps_map_finalize;
    object_class->constructor = osm_gps_map_constructor;
    object_class->set_property = osm_gps_map_set_property;
    object_class->get_property = osm_gps_map_get_property;

    widget_class = GTK_WIDGET_CLASS (klass);
    widget_class->draw = osm_gps_map_draw;
    widget_class->configure_event = osm_gps_map_configure;
    widget_class->button_press_event = osm_gps_map_button_press;
    widget_class->button_release_event = osm_gps_map_button_release;
    widget_class->motion_notify_event = osm_gps_map_motion_notify;
    widget_class->scroll_event = osm_gps_map_scroll_event;
    //widget_class->get_preferred_width = osm_gps_map_get_preferred_width;
    //widget_class->get_preferred_height = osm_gps_map_get_preferred_height;

    /* default implementation of draw_gps_point */
    klass->draw_gps_point = osm_gps_map_draw_gps_point;

    g_object_class_install_property (object_class,
                                     PROP_AUTO_CENTER,
                                     g_param_spec_boolean ("auto-center",
                                                           "auto center",
                                                           "map auto center",
                                                           TRUE,
                                                           G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    g_object_class_install_property (object_class,
                                     PROP_AUTO_CENTER_THRESHOLD,
                                     g_param_spec_float ("auto-center-threshold",
                                                         "auto center threshold",
                                                         "the amount of the window the gps point must move before auto centering",
                                                         0.0, /* minimum property value */
                                                         1.0, /* maximum property value */
                                                         0.25,
                                                         G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    g_object_class_install_property (object_class,
                                     PROP_RECORD_TRIP_HISTORY,
                                     g_param_spec_boolean ("record-trip-history",
                                                           "record trip history",
                                                           "should all gps points be recorded in a trip history",
                                                           TRUE,
                                                           G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    g_object_class_install_property (object_class,
                                     PROP_SHOW_TRIP_HISTORY,
                                     g_param_spec_boolean ("show-trip-history",
                                                           "show trip history",
                                                           "should the recorded trip history be shown on the map",
                                                           TRUE,
                                                           G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    /**
     * OsmGpsMap:show-gps-point:
     *
     * Controls whether the current gps point is shown on the map. Note that
     * for derived classes that implement the draw_gps_point vfunc, if this
     * property is %FALSE
     **/
    g_object_class_install_property (object_class,
                                     PROP_SHOW_GPS_POINT,
                                     g_param_spec_boolean ("show-gps-point",
                                                           "show gps point",
                                                           "should the current gps point be shown on the map",
                                                           TRUE,
                                                           G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    g_object_class_install_property (object_class,
                                     PROP_AUTO_DOWNLOAD,
                                     g_param_spec_boolean ("auto-download",
                                                           "auto download",
                                                           "map auto download",
                                                           TRUE,
                                                           G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    /**
     * OsmGpsMap:repo-uri:
     *
     * A URI string which defines the location and format to fetch tiles
     * for the map. The string is of the format
     * "http://tile.openstreetmap.org/&num;Z/&num;X/&num;Y.png". Characters
     * that begin with &num; are treated as tokens and replaced according to
     * the following rules;
     *
     * <itemizedlist>
     * <listitem>
     * <para>
     * \#X - X-tile, slippy map format
     * </para>
     * </listitem>
     * <listitem>
     * <para>
     * \#Y - Y-tile, slippy map format, mercator projection
     * </para>
     * </listitem>
     * <listitem>
     * <para>
     * \#Z - Zoom level, where min_zoom &gt;= zoom &lt;= max_zoom
     * </para>
     * </listitem>
     * <listitem>
     * <para>
     * \#S - Zoom level, where -max_zoom &gt;= (zoom-max_zoom) &lt;= min_zoom
     * </para>
     * </listitem>
     * <listitem>
     * <para>
     * \#Q - Quad tree format, set of "qrts"
     * </para>
     * </listitem>
     * <listitem>
     * <para>
     * \#Q0 - Quad tree format, set of "0123"
     * </para>
     * </listitem>
     * <listitem>
     * <para>
     * \#YS - Not Implemented
     * </para>
     * </listitem>
     * <listitem>
     * <para>
     * \#R - Random integer in range [0,4]
     * </para>
     * </listitem>
     * </itemizedlist>
     *
     * <note>
     * <para>
     * If you do not wish to use the default map tiles (provided by OpenStreeMap)
     * it is recommened that you use one of the predefined map sources, and thus
     * you should construct the map by setting #OsmGpsMap:map-source and not
     * #OsmGpsMap:repo-uri. The #OsmGpsMap:repo-uri property is primarily
     * designed for applications that wish complete control of tile repository
     * management, or wish to use #OsmGpsMap with a tile repository it does not
     * explicitly support.
     * </para>
     * </note>
     **/
    g_object_class_install_property (object_class,
                                     PROP_REPO_URI,
                                     g_param_spec_string ("repo-uri",
                                                          "repo uri",
                                                          "Map source tile repository uri",
                                                          OSM_REPO_URI,
                                                          G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

     g_object_class_install_property (object_class,
                                     PROP_PROXY_URI,
                                     g_param_spec_string ("proxy-uri",
                                                          "proxy uri",
                                                          "HTTP proxy uri or NULL",
                                                          NULL,
                                                          G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));


    /**
     * OsmGpsMap:tile-cache:
     *
     * Either a full path or one of the special format URIs
     * #OSM_GPS_MAP_CACHE_DISABLED, #OSM_GPS_MAP_CACHE_AUTO,
     * #OSM_GPS_MAP_CACHE_FRIENDLY. Also see #OsmGpsMap:tile-cache-base for a
     * full understanding.
     *
     * #OSM_GPS_MAP_CACHE_DISABLED disables the on disk tile cache (so all tiles
     * are fetched from the network. #OSM_GPS_MAP_CACHE_AUTO causes the tile
     * cache to be /tile-cache-base/md5(repo-uri), where md5 is the md5sum
     * of #OsmGpsMap:repo-uri. #OSM_GPS_MAP_CACHE_FRIENDLY
     * causes the tile cache to be /tile-cache-base/friendlyname(repo-uri).
     *
     * Any other string is interpreted as a local path, i.e. /path/to/cache
     **/
    g_object_class_install_property (object_class,
                                     PROP_TILE_CACHE_DIR,
                                     g_param_spec_string ("tile-cache",
                                                          "tile cache",
                                                          "Tile cache dir",
                                                          OSM_GPS_MAP_CACHE_AUTO,
                                                          G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    /**
     * OsmGpsMap:tile-cache-base:
     *
     * The base directory of the tile cache when you have constructed
     * the map with #OsmGpsMap:tile-cache set to #OSM_GPS_MAP_CACHE_AUTO or
     * #OSM_GPS_MAP_CACHE_FRIENDLY
     *
     * The string is interpreted as a local path, i.e. /path/to/cache. If NULL
     * is supplied, map tiles are cached starting in the users cache directory,
     * (as outlined in the
     * <ulink url="http://www.freedesktop.org/wiki/Specifications/basedir-spec">
     * <citetitle>XDG Base Directory Specification</citetitle></ulink>). To get the
     * base directory where map tiles will be cached call
     * osm_gps_map_get_default_cache_directory()
     *
     **/
    g_object_class_install_property (object_class,
                                     PROP_TILE_CACHE_BASE_DIR,
                                     g_param_spec_string ("tile-cache-base",
                                                          "tile cache-base",
                                                          "Base directory to which friendly and auto paths are appended",
                                                          NULL,
                                                          G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    /**
     * OsmGpsMap:zoom:
     *
     * The map zoom level. Connect to ::notify::zoom if you want to be informed
     * when this changes.
    **/
    g_object_class_install_property (object_class,
                                     PROP_ZOOM,
                                     g_param_spec_int ("zoom",
                                                       "zoom",
                                                       "Map zoom level",
                                                       MIN_ZOOM, /* minimum property value */
                                                       MAX_ZOOM, /* maximum property value */
                                                       3,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property (object_class,
                                     PROP_TILE_ZOOM_OFFSET,
                                     g_param_spec_int ("tile-zoom-offset",
                                                       "tile zoom-offset",
                                                       "Number of zoom-levels to upsample tiles",
                                                       MIN_TILE_ZOOM_OFFSET, /* minimum propery value */
                                                       MAX_TILE_ZOOM_OFFSET, /* maximum propery value */
                                                       0,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property (object_class,
                                     PROP_MAX_ZOOM,
                                     g_param_spec_int ("max-zoom",
                                                       "max zoom",
                                                       "Maximum zoom level",
                                                       MIN_ZOOM, /* minimum property value */
                                                       MAX_ZOOM, /* maximum property value */
                                                       OSM_MAX_ZOOM,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property (object_class,
                                     PROP_MIN_ZOOM,
                                     g_param_spec_int ("min-zoom",
                                                       "min zoom",
                                                       "Minimum zoom level",
                                                       MIN_ZOOM, /* minimum property value */
                                                       MAX_ZOOM, /* maximum property value */
                                                       OSM_MIN_ZOOM,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property (object_class,
                                     PROP_LATITUDE,
                                     g_param_spec_float ("latitude",
                                                         "latitude",
                                                         "Latitude in degrees",
                                                         -90.0, /* minimum property value */
                                                         90.0, /* maximum property value */
                                                         0,
                                                         G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_LONGITUDE,
                                     g_param_spec_float ("longitude",
                                                         "longitude",
                                                         "Longitude in degrees",
                                                         -180.0, /* minimum property value */
                                                         180.0, /* maximum property value */
                                                         0,
                                                         G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_MAP_X,
                                     g_param_spec_int ("map-x",
                                                       "map-x",
                                                       "Initial map x location",
                                                       G_MININT, /* minimum property value */
                                                       G_MAXINT, /* maximum property value */
                                                       890,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property (object_class,
                                     PROP_MAP_Y,
                                     g_param_spec_int ("map-y",
                                                       "map-y",
                                                       "Initial map y location",
                                                       G_MININT, /* minimum property value */
                                                       G_MAXINT, /* maximum property value */
                                                       515,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    /**
     * OsmGpsMap:tiles-queued:
     *
     * The number of tiles currently waiting to download. Connect to
     * ::notify::tiles-queued if you want to be informed when this changes
    **/
    g_object_class_install_property (object_class,
                                     PROP_TILES_QUEUED,
                                     g_param_spec_int ("tiles-queued",
                                                       "tiles-queued",
                                                       "The number of tiles currently waiting to download",
                                                       G_MININT, /* minimum property value */
                                                       G_MAXINT, /* maximum property value */
                                                       0,
                                                       G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_GPS_TRACK_WIDTH,
                                     g_param_spec_float ("gps-track-width",
                                                         "gps-track-width",
                                                         "The width of the lines drawn for the gps track",
                                                         1.0,       /* minimum property value */
                                                         100.0,     /* maximum property value */
                                                         4.0,
                                                         G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    g_object_class_install_property (object_class,
                                     PROP_GPS_POINT_R1,
                                     g_param_spec_int ("gps-track-point-radius",
                                                       "gps-track-point-radius",
                                                       "The radius of the gps point inner circle",
                                                       0,           /* minimum property value */
                                                       G_MAXINT,    /* maximum property value */
                                                       5,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    g_object_class_install_property (object_class,
                                     PROP_GPS_POINT_R2,
                                     g_param_spec_int ("gps-track-highlight-radius",
                                                       "gps-track-highlight-radius",
                                                       "The radius of the gps point highlight circle",
                                                       0,           /* minimum property value */
                                                       G_MAXINT,    /* maximum property value */
                                                       20,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    /**
     * OsmGpsMap:map-source:
     *
     * A #OsmGpsMapSource_t representing the tile repository to use
     *
     * <note>
     * <para>
     * If you do not wish to use the default map tiles (provided by OpenStreeMap)
     * it is recommened that you set this property at construction, instead
     * of setting #OsmGpsMap:repo-uri.
     * </para>
     * </note>
     **/
    g_object_class_install_property (object_class,
                                     PROP_MAP_SOURCE,
                                     g_param_spec_int ("map-source",
                                                       "map source",
                                                       "The map source ID",
                                                       -1,          /* minimum property value */
                                                       G_MAXINT,    /* maximum property value */
                                                       -1,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    g_object_class_install_property (object_class,
                                     PROP_IMAGE_FORMAT,
                                     g_param_spec_string ("image-format",
                                                          "image format",
                                                          "The map source tile repository image format (jpg, png)",
                                                          OSM_IMAGE_FORMAT,
                                                          G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property (object_class,
                                     PROP_DRAG_LIMIT,
                                     g_param_spec_int ("drag-limit",
                                                       "drag limit",
                                                       "The number of pixels the user has to move the pointer in order to start dragging",
                                                       0,           /* minimum property value */
                                                       G_MAXINT,    /* maximum property value */
                                                       10,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    /**
     * OsmGpsMap::changed:
     *
     * The #OsmGpsMap::changed signal is emitted any time the map zoom or map center
     * is chaged (such as by dragging or zooming).
     *
     * <note>
     * <para>
     * If you are only interested in the map zoom, then you can simply connect
     * to ::notify::zoom
     * </para>
     * </note>
     **/
    g_signal_new ("changed", OSM_TYPE_GPS_MAP,
                  G_SIGNAL_RUN_FIRST, 0, NULL, NULL,
                  g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);
}

/**
 * osm_gps_map_download_maps:
 *
 * Downloads all tiles over the supplied zoom range in the rectangular
 * region specified by pt1 (north west corner) to pt2 (south east corner)
 *
 **/
void
osm_gps_map_download_maps (OsmGpsMap *map, OsmGpsMapPoint *pt1, OsmGpsMapPoint *pt2, int zoom_start, int zoom_end)
{
    OsmGpsMapPrivate *priv = map->priv;

    if (pt1 && pt2) {
        gchar *filename;
        int i,j,zoom;
        int num_tiles = 0;
        zoom_end = CLAMP(zoom_end, priv->min_zoom, priv->max_zoom);
        zoom_start = CLAMP(zoom_start, priv->min_zoom, priv->max_zoom);

        for(zoom=zoom_start; zoom<=zoom_end; zoom++) {
            int x1,y1,x2,y2;

            x1 = (int)floor((float)lon2pixel(zoom, pt1->rlon) / (float)TILESIZE);
            y1 = (int)floor((float)lat2pixel(zoom, pt1->rlat) / (float)TILESIZE);

            x2 = (int)floor((float)lon2pixel(zoom, pt2->rlon) / (float)TILESIZE);
            y2 = (int)floor((float)lat2pixel(zoom, pt2->rlat) / (float)TILESIZE);

            /* check for insane ranges */
            if ( (x2-x1) * (y2-y1) > MAX_DOWNLOAD_TILES ) {
                g_warning("Aborting download of zoom level %d and up, because "
                          "number of tiles would exceed %d", zoom, MAX_DOWNLOAD_TILES);
                break;
            }

            /* loop x1-x2 */
            for(i=x1; i<=x2; i++) {
                /* loop y1 - y2 */
                for(j=y1; j<=y2; j++) {
                    /* x = i, y = j */
                    filename = g_strdup_printf("%s%c%d%c%d%c%d.%s",
                                    priv->cache_dir, G_DIR_SEPARATOR,
                                    zoom, G_DIR_SEPARATOR,
                                    i, G_DIR_SEPARATOR,
                                    j,
                                    priv->image_format);
                    if (!g_file_test(filename, G_FILE_TEST_EXISTS)) {
                        osm_gps_map_download_tile(map, zoom, i, j, FALSE);
                        num_tiles++;
                    }
                    g_free(filename);
                }
            }
            g_debug("DL @Z:%d = %d tiles", zoom, num_tiles);
        }
    }
}

static void
cancel_message (char *key, SoupMessage *value, SoupSession *user_data)
{
    soup_session_cancel_message (user_data, value, SOUP_STATUS_CANCELLED);
}

/**
 * osm_gps_map_download_cancel_all:
 *
 * Cancels all tiles currently being downloaded. Typically used if you wish to
 * cacel a large number of tiles queued using osm_gps_map_download_maps()
 *
 * Since: 0.7.0
 **/
void
osm_gps_map_download_cancel_all (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;
    g_hash_table_foreach (priv->tile_queue, (GHFunc)cancel_message, priv->soup_session);
}

/**
 * osm_gps_map_get_bbox:
 * @pt1: (out): point to be filled with the top left location
 * @pt2: (out): point to be filled with the bottom right location
 *
 * Returns the geographic locations of the bounding box describing the contents
 * of the current window, i.e the top left and bottom right corners.
 **/
void
osm_gps_map_get_bbox (OsmGpsMap *map, OsmGpsMapPoint *pt1, OsmGpsMapPoint *pt2)
{
    GtkAllocation allocation;
    OsmGpsMapPrivate *priv = map->priv;

    if (pt1 && pt2) {
        gtk_widget_get_allocation(GTK_WIDGET(map), &allocation);
        pt1->rlat = pixel2lat(priv->map_zoom, priv->map_y);
        pt1->rlon = pixel2lon(priv->map_zoom, priv->map_x);
        pt2->rlat = pixel2lat(priv->map_zoom, priv->map_y + allocation.height);
        pt2->rlon = pixel2lon(priv->map_zoom, priv->map_x + allocation.width);
    }
}

/**
 * osm_gps_map_set_center_and_zoom:
 *
 * Since: 0.7.0
 **/
void osm_gps_map_set_center_and_zoom (OsmGpsMap *map, float latitude, float longitude, int zoom)
{
    osm_gps_map_set_center (map, latitude, longitude);
    osm_gps_map_set_zoom (map, zoom);
}

/**
 * osm_gps_map_set_center:
 *
 **/
void
osm_gps_map_set_center (OsmGpsMap *map, float latitude, float longitude)
{
    int pixel_x, pixel_y;
    OsmGpsMapPrivate *priv;
    GtkAllocation allocation;

    g_return_if_fail (OSM_IS_GPS_MAP (map));

    priv = map->priv;
    gtk_widget_get_allocation(GTK_WIDGET(map), &allocation);
    g_object_set(G_OBJECT(map), "auto-center", FALSE, NULL);

    priv->center_rlat = deg2rad(latitude);
    priv->center_rlon = deg2rad(longitude);

    pixel_x = lon2pixel(priv->map_zoom, priv->center_rlon);
    pixel_y = lat2pixel(priv->map_zoom, priv->center_rlat);

    priv->map_x = pixel_x - allocation.width/2;
    priv->map_y = pixel_y - allocation.height/2;

    osm_gps_map_map_redraw_idle(map);

    g_signal_emit_by_name(map, "changed");
}

/**
 * osm_gps_map_set_zoom_offset:
 *
 **/
void
osm_gps_map_set_zoom_offset (OsmGpsMap *map, int zoom_offset)
{
    OsmGpsMapPrivate *priv;

    g_return_if_fail (OSM_GPS_MAP (map));
    priv = map->priv;

    if (zoom_offset != priv->tile_zoom_offset)
    {
        priv->tile_zoom_offset = zoom_offset;
        osm_gps_map_map_redraw_idle (map);
    }
}

/**
 * osm_gps_map_set_zoom:
 *
 **/
int 
osm_gps_map_set_zoom (OsmGpsMap *map, int zoom)
{
    int width_center, height_center;
    OsmGpsMapPrivate *priv;
    GtkAllocation allocation;

    g_return_val_if_fail (OSM_IS_GPS_MAP (map), 0);
    priv = map->priv;

    if (zoom != priv->map_zoom)
    {
        gtk_widget_get_allocation(GTK_WIDGET(map), &allocation);
        width_center  = allocation.width / 2;
        height_center = allocation.height / 2;

        /* update zoom but constrain [min_zoom..max_zoom] */
        priv->map_zoom = CLAMP(zoom, priv->min_zoom, priv->max_zoom);
        priv->map_x = lon2pixel(priv->map_zoom, priv->center_rlon) - width_center;
        priv->map_y = lat2pixel(priv->map_zoom, priv->center_rlat) - height_center;

        osm_gps_map_map_redraw_idle(map);

        g_signal_emit_by_name(map, "changed");
        g_object_notify(G_OBJECT(map), "zoom");
    }
    return priv->map_zoom;
}

/**
 * osm_gps_map_zoom_in:
 *
 **/
int
osm_gps_map_zoom_in (OsmGpsMap *map)
{
    g_return_val_if_fail (OSM_IS_GPS_MAP (map), 0);
    return osm_gps_map_set_zoom(map, map->priv->map_zoom+1);
}

/**
 * osm_gps_map_zoom_out:
 *
 **/
int
osm_gps_map_zoom_out (OsmGpsMap *map)
{
    g_return_val_if_fail (OSM_IS_GPS_MAP (map), 0);
    return osm_gps_map_set_zoom(map, map->priv->map_zoom-1);
}

/**
 * osm_gps_map_new:
 *
 * Returns a new #OsmGpsMap object, defaults to showing data from
 * <ulink url="http://www.openstreetmap.org"><citetitle>OpenStreetMap</citetitle></ulink>
 *
 * See the properties description for more information about construction
 * parameters than could be passed to g_object_new()
 *
 * Returns: a newly created #OsmGpsMap object.
 **/
GtkWidget *
osm_gps_map_new (void)
{
    return g_object_new (OSM_TYPE_GPS_MAP, NULL);
}

/**
 * osm_gps_map_scroll:
 * @map:
 * @dx:
 * @dy:
 *
 * Scrolls the map by @dx, @dy pixels (positive north, east)
 *
 **/
void
osm_gps_map_scroll (OsmGpsMap *map, gint dx, gint dy)
{
    OsmGpsMapPrivate *priv;

    g_return_if_fail (OSM_IS_GPS_MAP (map));
    priv = map->priv;

    priv->map_x += dx;
    priv->map_y += dy;
    center_coord_update(map);

    osm_gps_map_map_redraw_idle (map);
}

/**
 * osm_gps_map_get_scale:
 * @map:
 *
 * Returns: the scale of the map at the center, in meters/pixel.
 *
 **/
float
osm_gps_map_get_scale (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv;

    g_return_val_if_fail (OSM_IS_GPS_MAP (map), OSM_GPS_MAP_INVALID);
    priv = map->priv;

    return osm_gps_map_get_scale_at_point(priv->map_zoom, priv->center_rlat, priv->center_rlon);
}

/**
 * osm_gps_map_get_default_cache_directory:
 *
 * Returns: the default cache directory for the library, that is the base
 * directory to which the full cache path is appended. If
 * #OsmGpsMap:tile-cache-base is omitted from the constructor then this value
 * is used.
 *
 **/
gchar *
osm_gps_map_get_default_cache_directory (void)
{
    return g_build_filename(
                        g_get_user_cache_dir(),
                        "osmgpsmap",
                        NULL);
}

/**
 * osm_gps_map_set_keyboard_shortcut:
 * @key: a #OsmGpsMapKey_t
 * @keyval:
 *
 * Associates a keyboard shortcut with the supplied @keyval
 * (as returned by #gdk_keyval_from_name or simiar). The action given in @key
 * will be triggered when the corresponding @keyval is pressed. By default
 * no keyboard shortcuts are associated.
 *
 **/
void
osm_gps_map_set_keyboard_shortcut (OsmGpsMap *map, OsmGpsMapKey_t key, guint keyval)
{
    g_return_if_fail (OSM_IS_GPS_MAP (map));
    g_return_if_fail(key < OSM_GPS_MAP_KEY_MAX);

    map->priv->keybindings[key] = keyval;
    map->priv->keybindings_enabled = TRUE;
}

/**
 * osm_gps_map_track_add:
 *
 * Since: 0.7.0
 **/
void
osm_gps_map_track_add (OsmGpsMap *map, OsmGpsMapTrack *track)
{
    OsmGpsMapPrivate *priv;

    g_return_if_fail (OSM_IS_GPS_MAP (map));
    priv = map->priv;

    g_object_ref(track);
    g_signal_connect(track, "point-added",
                    G_CALLBACK(on_gps_point_added), map);
    g_signal_connect(track, "notify",
                    G_CALLBACK(on_track_changed), map);

    priv->tracks = g_slist_append(priv->tracks, track);
    osm_gps_map_map_redraw_idle(map);
}

/**
 * osm_gps_map_track_remove_all:
 *
 * Since: 0.7.0
 **/
void
osm_gps_map_track_remove_all (OsmGpsMap *map)
{
    g_return_if_fail (OSM_IS_GPS_MAP (map));

    gslist_of_gobjects_free(&map->priv->tracks);
    osm_gps_map_map_redraw_idle(map);
}

/**
 * osm_gps_map_track_remove:
 *
 * Since: 0.7.0
 **/
gboolean
osm_gps_map_track_remove (OsmGpsMap *map, OsmGpsMapTrack *track)
{
    GSList *data;

    g_return_val_if_fail (OSM_IS_GPS_MAP (map), FALSE);
    g_return_val_if_fail (track != NULL, FALSE);

    data = gslist_remove_one_gobject (&map->priv->tracks, G_OBJECT(track));
    osm_gps_map_map_redraw_idle(map);
    return data != NULL;
}

/**
 * osm_gps_map_gps_clear:
 *
 * Since: 0.7.0
 **/
void
osm_gps_map_gps_clear (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv;

    g_return_if_fail (OSM_IS_GPS_MAP (map));
    priv = map->priv;

    g_object_unref(priv->gps_track);
    priv->gps_track = osm_gps_map_track_new();
    g_signal_connect(priv->gps_track, "point-added",
                    G_CALLBACK(on_gps_point_added), map);
    g_signal_connect(priv->gps_track, "notify",
                    G_CALLBACK(on_track_changed), map);
    osm_gps_map_map_redraw_idle(map);
}

/**
 * osm_gps_map_gps_get_track:
 *
 * Returns: (transfer none): The #OsmGpsMapTrack of the internal GPS track, 
 * i.e. that which is modified when calling osm_gps_map_gps_add(). You must 
 * not free this.
 * Since: 0.7.0
 **/
OsmGpsMapTrack *
osm_gps_map_gps_get_track (OsmGpsMap *map)
{
    g_return_val_if_fail (OSM_IS_GPS_MAP (map), NULL);
    return map->priv->gps_track;
}

/**
 * osm_gps_map_gps_add:
 * @latitude: degrees
 * @longitude: degrees
 * @heading: degrees or #OSM_GPS_MAP_INVALID to disable showing heading
 *
 * Since: 0.7.0
 **/
void
osm_gps_map_gps_add (OsmGpsMap *map, float latitude, float longitude, float heading)
{
    OsmGpsMapPrivate *priv;

    g_return_if_fail (OSM_IS_GPS_MAP (map));
    priv = map->priv;

    /* update the current point */
    priv->gps->rlat = deg2rad(latitude);
    priv->gps->rlon = deg2rad(longitude);
    priv->gps_track_used = TRUE;
    priv->gps_heading = deg2rad(heading);

    /* If trip marker add to list of gps points */
    if (priv->trip_history_record_enabled) {
        OsmGpsMapPoint point;
        osm_gps_map_point_set_degrees (&point, latitude, longitude);
        /* this will cause a redraw to be scheduled */
        osm_gps_map_track_add_point (priv->gps_track, &point);
    } else {
        osm_gps_map_map_redraw_idle (map);
        maybe_autocenter_map (map);
    }
}

/**
 * osm_gps_map_image_add:
 *
 * Returns: (transfer full): A #OsmGpsMapImage representing the added pixbuf
 * Since: 0.7.0
 **/
OsmGpsMapImage *
osm_gps_map_image_add (OsmGpsMap *map, float latitude, float longitude, GdkPixbuf *image)
{
    return osm_gps_map_image_add_with_alignment_z (map, latitude, longitude, image, 0.5, 0.5, 0);
}

/**
 * osm_gps_map_image_add_z:
 *
 * Returns: (transfer full): A #OsmGpsMapImage representing the added pixbuf
 * Since: 0.7.4
 **/
OsmGpsMapImage *
osm_gps_map_image_add_z (OsmGpsMap *map, float latitude, float longitude, GdkPixbuf *image, gint zorder)
{
    return osm_gps_map_image_add_with_alignment_z (map, latitude, longitude, image, 0.5, 0.5, zorder);
}

static void
on_image_changed (OsmGpsMapImage *image, GParamSpec *pspec, OsmGpsMap *map)
{
    osm_gps_map_map_redraw_idle (map);
}

/**
 * osm_gps_map_image_add_with_alignment:
 *
 * Returns: (transfer full): A #OsmGpsMapImage representing the added pixbuf
 * Since: 0.7.0
 **/
OsmGpsMapImage *
osm_gps_map_image_add_with_alignment (OsmGpsMap *map, float latitude, float longitude, GdkPixbuf *image, float xalign, float yalign)
{
    return osm_gps_map_image_add_with_alignment_z (map, latitude, longitude, image, xalign, yalign, 0);
}

static gint
osm_gps_map_image_z_compare(gconstpointer item1, gconstpointer item2)
{
    gint z1 = osm_gps_map_image_get_zorder(OSM_GPS_MAP_IMAGE(item1));
    gint z2 = osm_gps_map_image_get_zorder(OSM_GPS_MAP_IMAGE(item2));

    return(z1 - z2 + 1);
}

/**
 * osm_gps_map_image_add_with_alignment_z:
 *
 * Returns: (transfer full): A #OsmGpsMapImage representing the added pixbuf
 * Since: 0.7.4
 **/
OsmGpsMapImage *
osm_gps_map_image_add_with_alignment_z (OsmGpsMap *map, float latitude, float longitude, GdkPixbuf *image, float xalign, float yalign, gint zorder)
{
    OsmGpsMapImage *im;
    OsmGpsMapPoint pt;

    g_return_val_if_fail (OSM_IS_GPS_MAP (map), NULL);
    pt.rlat = deg2rad(latitude);
    pt.rlon = deg2rad(longitude);

    im = g_object_new (OSM_TYPE_GPS_MAP_IMAGE, "pixbuf", image, "x-align", xalign, "y-align", yalign, "point", &pt, "z-order", zorder, NULL);
    g_signal_connect(im, "notify",
                    G_CALLBACK(on_image_changed), map);

    map->priv->images = g_slist_insert_sorted(map->priv->images, im,
                                              (GCompareFunc) osm_gps_map_image_z_compare);
    osm_gps_map_map_redraw_idle(map);

    g_object_ref(im);
    return im;
}

/**
 * osm_gps_map_image_remove:
 *
 * Since: 0.7.0
 **/
gboolean
osm_gps_map_image_remove (OsmGpsMap *map, OsmGpsMapImage *image)
{
    GSList *data;

    g_return_val_if_fail (OSM_IS_GPS_MAP (map), FALSE);
    g_return_val_if_fail (image != NULL, FALSE);

    data = gslist_remove_one_gobject (&map->priv->images, G_OBJECT(image));
    osm_gps_map_map_redraw_idle(map);
    return data != NULL;
}

/**
 * osm_gps_map_image_remove_all:
 *
 * Since: 0.7.0
 **/
void
osm_gps_map_image_remove_all (OsmGpsMap *map)
{
    g_return_if_fail (OSM_IS_GPS_MAP (map));

    gslist_of_gobjects_free(&map->priv->images);
    osm_gps_map_map_redraw_idle(map);
}

/**
 * osm_gps_map_layer_add:
 * @layer: a #OsmGpsMapLayer object
 *
 * Since: 0.7.0
 **/
void
osm_gps_map_layer_add (OsmGpsMap *map, OsmGpsMapLayer *layer)
{
    g_return_if_fail (OSM_IS_GPS_MAP (map));
    g_return_if_fail (OSM_GPS_MAP_IS_LAYER (layer));

    g_object_ref(G_OBJECT(layer));
    map->priv->layers = g_slist_append(map->priv->layers, layer);
}

/**
 * osm_gps_map_layer_remove:
 * @layer: a #OsmGpsMapLayer object
 *
 * Since: 0.7.0
 **/
gboolean
osm_gps_map_layer_remove (OsmGpsMap *map, OsmGpsMapLayer *layer)
{
    GSList *data;

    g_return_val_if_fail (OSM_IS_GPS_MAP (map), FALSE);
    g_return_val_if_fail (layer != NULL, FALSE);

    data = gslist_remove_one_gobject (&map->priv->layers, G_OBJECT(layer));
    osm_gps_map_map_redraw_idle(map);
    return data != NULL;
}

/**
 * osm_gps_map_layer_remove_all:
 *
 * Since: 0.7.0
 **/
void
osm_gps_map_layer_remove_all (OsmGpsMap *map)
{
    g_return_if_fail (OSM_IS_GPS_MAP (map));

    gslist_of_gobjects_free(&map->priv->layers);
    osm_gps_map_map_redraw_idle(map);
}

/**
 * osm_gps_map_convert_screen_to_geographic:
 * @map:
 * @pixel_x: pixel location on map, x axis
 * @pixel_y: pixel location on map, y axis
 * @pt: (out): location 
 *
 * Convert the given pixel location on the map into corresponding
 * location on the globe
 *
 * Since: 0.7.0
 **/
void
osm_gps_map_convert_screen_to_geographic(OsmGpsMap *map, gint pixel_x, gint pixel_y, OsmGpsMapPoint *pt)
{
    OsmGpsMapPrivate *priv;
    int map_x0, map_y0;

    g_return_if_fail (OSM_IS_GPS_MAP (map));
    g_return_if_fail (pt);

    priv = map->priv;
    map_x0 = priv->map_x - EXTRA_BORDER;
    map_y0 = priv->map_y - EXTRA_BORDER;

    pt->rlat = pixel2lat(priv->map_zoom, map_y0 + pixel_y);
    pt->rlon = pixel2lon(priv->map_zoom, map_x0 + pixel_x);
}

/**
 * osm_gps_map_convert_geographic_to_screen:
 * @map:
 * @pt: location
 * @pixel_x: (out): pixel location on map, x axis
 * @pixel_y: (out): pixel location on map, y axis
 *
 * Convert the given location on the globe to the corresponding
 * pixel locations on the map.
 *
 * Since: 0.7.0
 **/
void
osm_gps_map_convert_geographic_to_screen(OsmGpsMap *map, OsmGpsMapPoint *pt, gint *pixel_x, gint *pixel_y)
{
    OsmGpsMapPrivate *priv;
    int map_x0, map_y0;

    g_return_if_fail (OSM_IS_GPS_MAP (map));
    g_return_if_fail (pt);

    priv = map->priv;
    map_x0 = priv->map_x - EXTRA_BORDER;
    map_y0 = priv->map_y - EXTRA_BORDER;

    if (pixel_x)
        *pixel_x = lon2pixel(priv->map_zoom, pt->rlon) - map_x0 + priv->drag_mouse_dx;
    if (pixel_y)
        *pixel_y = lat2pixel(priv->map_zoom, pt->rlat) - map_y0 + priv->drag_mouse_dy;
}

/**
 * osm_gps_map_get_event_location:
 * @map:
 * @event: A #GtkEventButton that occured on the map
 *
 * A convenience function for getting the geographic location of events,
 * such as mouse clicks, on the map
 *
 * Returns: (transfer full): The point on the globe corresponding to the click
 * Since: 0.7.0
 **/
OsmGpsMapPoint *
osm_gps_map_get_event_location (OsmGpsMap *map, GdkEventButton *event)
{
    OsmGpsMapPoint *p = osm_gps_map_point_new_degrees(0.0,0.0);
    osm_gps_map_convert_screen_to_geographic(map, event->x, event->y, p);
    return p;
}

