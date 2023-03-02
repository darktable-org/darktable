/*
    This file is part of darktable,
    Copyright (C) 2012-2022 darktable developers.

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
#include "common/debug.h"
#include "common/file_location.h"
#include "common/image_cache.h"
#include "common/collection.h"
#include "common/selection.h"
#include "common/gpx.h"
#include "common/geo.h"
#include "common/datetime.h"
#include "control/conf.h"
#include "dtgtk/button.h"
#include "control/jobs.h"
#include "gui/accelerators.h"
#include "libs/lib_api.h"
#ifdef HAVE_MAP
#include "views/view.h"
#endif
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef struct tz_tuple_t
{
  char *name, *display;
} tz_tuple_t;

#define DT_GEOTAG_PARTS_NB 7

typedef struct dt_lib_datetime_t
{
  GtkWidget *widget[DT_GEOTAG_PARTS_NB];
  GtkWidget *sign;
} dt_lib_datetime_t;

#ifdef HAVE_MAP
typedef struct dt_lib_tracks_data_t
{
  GObject *track;
  dt_map_box_t map_box;
} dt_lib_tracks_data_t;

typedef struct dt_lib_tracks_t
{
  dt_lib_tracks_data_t td[1];
} dt_lib_tracks_t;

typedef enum dt_tracks_cols_t
{
  DT_GEO_TRACKS_ACTIVE = 0,       // active / deactivated track
  DT_GEO_TRACKS_DATETIME,         // displayed start datetime
  DT_GEO_TRACKS_POINTS,           // nb points
  DT_GEO_TRACKS_IMAGES,           // nb images
  DT_GEO_TRACKS_SEGID,            // id track segment
  DT_GEO_TRACKS_TOOLTIP,          // datetime details
  DT_GEO_TRACKS_NUM_COLS
} dt_tracks_cols_t;

#endif

typedef struct dt_lib_geotagging_t
{
  dt_lib_datetime_t dt;
  dt_lib_datetime_t dt0;
  dt_lib_datetime_t of;
  GDateTime *datetime;
  GDateTime *datetime0;
  GTimeSpan offset;
  gboolean editing;
  uint32_t imgid;
  GList* imgs;
  int nb_imgs;
  GtkWidget *apply_offset;
  GtkWidget *lock_offset;
  GtkWidget *apply_datetime;
  GtkWidget *timezone;
  GList *timezones;
  GtkWidget *timezone_changed;
  GtkWidget *gpx_button;
  GTimeZone *tz_camera;
#ifdef HAVE_MAP
  struct
  {
    gboolean view;
    GtkWidget *gpx_button, *gpx_file, *gpx_view;
    struct dt_gpx_t *gpx;
    dt_lib_tracks_t *tracks;
    dt_map_box_t map_box;
    int nb_tracks, nb_imgs;
    GtkWidget *gpx_section, *preview_button, *apply_gpx_button,
              *select_button, *nb_imgs_label;
    GtkTreeViewColumn *sel_tracks;
  } map;
#endif
} dt_lib_geotagging_t;

typedef struct dt_sel_img_t
{
  uint32_t imgid;
  uint32_t segid;
  gchar dt[DT_DATETIME_LENGTH];
  gboolean counted;
  dt_image_geoloc_t gl;
  GObject *image;
} dt_sel_img_t;

static void _datetime_entry_changed(GtkWidget *entry, dt_lib_module_t *self);
static void _setup_selected_images_list(dt_lib_module_t *self);

static void free_tz_tuple(gpointer data)
{
  tz_tuple_t *tz_tuple = (tz_tuple_t *)data;
  g_free(tz_tuple->display);
#ifdef _WIN32
  g_free(tz_tuple->name); // on non-Windows both point to the same string
#endif
  free(tz_tuple);
}

const char *name(dt_lib_module_t *self)
{
  return _("geotagging");
}

const char **views(dt_lib_module_t *self)
{
#ifdef HAVE_MAP
  static const char *v[] = {"lighttable", "map", NULL};
#else
  static const char *v[] = {"lighttable", NULL};
#endif
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 450;
}

// modify the datetime_taken field in the db/cache of selected images
static void _apply_offset_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  if(d->offset)
    dt_control_datetime(d->offset, NULL, NULL);
}

// modify the datetime_taken field in the db/cache of selected images
static void _apply_datetime_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  if(d->datetime)
  {
    char dt[DT_DATETIME_LENGTH];
    dt_datetime_gdatetime_to_exif(dt, sizeof(dt), d->datetime);
    dt_control_datetime(0, dt, NULL);
  }
}

static gboolean _lib_geotagging_filter_gpx(const GtkFileFilterInfo *filter_info, gpointer data)
{
  if(!g_ascii_strcasecmp(filter_info->mime_type, "application/gpx+xml")) return TRUE;

  const gchar *filename = filter_info->filename;
  const char *cc = filename + strlen(filename);
  for(; *cc != '.' && cc > filename; cc--)
    ;

  if(!g_ascii_strcasecmp(cc, ".gpx")) return TRUE;

  return FALSE;
}

static GtkWidget *_set_up_label(const char *name, const int align, GtkWidget *grid,
                                const int col, const int line, const int ellipsize)
{
  GtkWidget *label = gtk_label_new(name);
  gtk_label_set_ellipsize(GTK_LABEL(label), ellipsize);
  if(ellipsize != PANGO_ELLIPSIZE_NONE)
    gtk_widget_set_visible(label, TRUE);
  gtk_widget_set_halign(label, align);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_grid_attach(GTK_GRID(grid), label, col, line, 1, 1);
  return label;
}

static gchar *_utc_timeval_to_localtime_text(GDateTime *utc_dt, GTimeZone *tz_camera,
                                             const gboolean full)
{
  GDateTime *local_dt  = g_date_time_to_timezone(utc_dt, tz_camera);
  gchar *dts = g_date_time_format(local_dt, full ? "%Y:%m:%d %H:%M:%S" : "%H:%M:%S");
  g_date_time_unref(local_dt);
  return dts;
}

static GDateTime *_localtime_text_to_utc_timeval(const char *date_time,
                                                 GTimeZone *tz_camera, GTimeZone *tz_utc,
                                                 GTimeSpan offset)
{
  GDateTime *exif_time = dt_datetime_exif_to_gdatetime(date_time, tz_camera);
  GDateTime *dt_offset = g_date_time_add(exif_time, offset);
  GDateTime *utc_time = g_date_time_to_timezone(dt_offset, tz_utc);

  g_date_time_unref(exif_time);
  g_date_time_unref(dt_offset);
  return utc_time;
}

static int _count_images_per_track(dt_gpx_track_segment_t *t, dt_gpx_track_segment_t *n,
                                   dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;

  int nb_imgs = 0;
  for(GList *i = d->imgs; i; i = g_list_next(i))
  {
    dt_sel_img_t *im = (dt_sel_img_t *)i->data;
    if(im->segid == -1)
    {
      GDateTime *dt = _localtime_text_to_utc_timeval(im->dt, d->tz_camera, darktable.utc_tz, d->offset);
      if((g_date_time_compare(dt, t->start_dt) >= 0
          && g_date_time_compare(dt, t->end_dt) <= 0)
         || (n && g_date_time_compare(dt, t->end_dt) >= 0
             && g_date_time_compare(dt, n->start_dt) <= 0))
      {
        nb_imgs++;
        im->segid = t->id;
      }
      g_date_time_unref(dt);
    }
  }
  return nb_imgs;
}

#ifdef HAVE_MAP
static gchar *_utc_timeval_to_utc_text(GDateTime *utc_dt, const gboolean full)
{
  gchar *dts = g_date_time_format(utc_dt, full ? "%Y:%m:%d %H:%M:%S" : "%H:%M:%S");
  return dts;
}

static gchar *_datetime_tooltip(GDateTime *start, GDateTime *end, GTimeZone *tz)
{
  gchar *dtsl = _utc_timeval_to_localtime_text(start, tz, FALSE);
  gchar *dtel = _utc_timeval_to_localtime_text(end, tz, FALSE);
  gchar *dtsu = _utc_timeval_to_utc_text(start, FALSE);
  gchar *dteu = _utc_timeval_to_utc_text(end, FALSE);
  gchar *res = g_strdup_printf("%s -> %s LT\n%s -> %s UTC", dtsl, dtel, dtsu, dteu);
  g_free(dtsl);
  g_free(dtel);
  g_free(dtsu);
  g_free(dteu);
  return res;
}

static void _remove_images_from_map(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  for(GList *i = d->imgs; i; i = g_list_next(i))
  {
    dt_sel_img_t *im = (dt_sel_img_t *)i->data;
    if(im->image)
    {
      dt_view_map_remove_marker(darktable.view_manager, MAP_DISPLAY_THUMB, im->image);
      im->image = NULL;
    }
  }
}

static void _refresh_images_displayed_on_track(const int segid, const gboolean active, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  for(GList *i = d->imgs; i; i = g_list_next(i))
  {
    dt_sel_img_t *im = (dt_sel_img_t *)i->data;
    if(im->segid == segid && active)
    {
      GDateTime *dt = _localtime_text_to_utc_timeval(im->dt, d->tz_camera, darktable.utc_tz, d->offset);
      if(!dt_gpx_get_location(d->map.gpx, dt, &im->gl))
        im->gl.latitude = NAN;
      g_date_time_unref(dt);
    }
    else if(im->segid == segid && !active && im->image)
    {
      dt_view_map_remove_marker(darktable.view_manager, MAP_DISPLAY_THUMB, im->image);
      im->image = NULL;
      im->gl.latitude = NAN;
    }
  }
  int count = 0;
  for(GList *i = d->imgs; active && i; i = g_list_next(i))
  {
    dt_sel_img_t *im = (dt_sel_img_t *)i->data;
    if(im->segid == segid && im->gl.latitude != NAN)
    {
      count++;
      dt_sel_img_t *next = i->next ? (dt_sel_img_t *)i->next->data
                                   : NULL;
      if(!im->image && (!next
                        || !((next->gl.latitude == im->gl.latitude)
                             && (next->gl.longitude == im->gl.longitude))))
      {
        struct {uint32_t imgid; float latitude; float longitude; int count;} p;
        p.imgid = im->imgid;
        p.latitude = im->gl.latitude;
        p.longitude = im->gl.longitude;
        p.count = count == 1 ? 0 : count;
        GList *img = g_list_prepend(NULL, &p);
        im->image = dt_view_map_add_marker(darktable.view_manager, MAP_DISPLAY_THUMB, img);
        g_list_free(img);
        count = 0;
      }
    }
  }
}

static void _update_nb_images(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->map.gpx_view));
  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
  int nb_imgs = 0;
  for(int segid = 0; valid && segid < d->map.nb_tracks; segid++)
  {
    gboolean active;
    int nb;
    gtk_tree_model_get(model, &iter, DT_GEO_TRACKS_ACTIVE, &active,
                                     DT_GEO_TRACKS_IMAGES, &nb, -1);
    if(active)
      nb_imgs += nb;
    valid = gtk_tree_model_iter_next(model, &iter);
  }
  d->map.nb_imgs = nb_imgs;
  gchar *nb = g_strdup_printf("%d/%d", nb_imgs, d->nb_imgs);
  gtk_label_set_text(GTK_LABEL(d->map.nb_imgs_label), nb);
  g_free(nb);
}

static void _update_buttons(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  gtk_widget_set_sensitive(d->map.preview_button, d->map.nb_tracks);
  GtkWidget *label = gtk_bin_get_child(GTK_BIN(d->map.apply_gpx_button));
  gtk_label_set_text(GTK_LABEL(label), d->offset ? _("apply offset and geo-location")
                                                 : _("apply geo-location"));
  gtk_widget_set_tooltip_text(d->map.apply_gpx_button,
                              d->offset ? _("apply offset and geo-location to matching images"
                                            "\ndouble operation: two ctrl-Z to undo")
                                        : _("apply geo-location to matching images"));
  gtk_widget_set_sensitive(d->map.apply_gpx_button, d->map.nb_imgs);
  gtk_widget_set_sensitive(d->map.select_button,
                           d->map.nb_imgs && d->map.nb_imgs != d->nb_imgs);
}

static GList *_get_images_on_active_tracks(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->map.gpx_view));
  GtkTreeIter iter;
  int segid = 0;
  GList *imgs = NULL;
  gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
  while(valid)
  {
    gboolean active;
    gtk_tree_model_get(model, &iter, DT_GEO_TRACKS_ACTIVE, &active, -1);
    if(active)
    {
      for(GList *i = d->imgs; i; i = g_list_next(i))
      {
        dt_sel_img_t *im = (dt_sel_img_t *)i->data;
        if(im->segid == segid)
          imgs = g_list_prepend(imgs, GINT_TO_POINTER(im->imgid));
      }
    }
    valid = gtk_tree_model_iter_next(model, &iter);
    segid++;
  }
  return imgs;
}

static void _refresh_displayed_images(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->map.gpx_view));
  GtkTreeIter iter;
  const gboolean preview = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->map.preview_button));
  gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
  for(int segid = 0; valid && segid < d->map.nb_tracks; segid++)
  {
    gboolean active;
    gtk_tree_model_get(model, &iter, DT_GEO_TRACKS_ACTIVE, &active, -1);
    _refresh_images_displayed_on_track(segid, active && preview, self);
    valid = gtk_tree_model_iter_next(model, &iter);
  }
}

static gboolean _update_map_box(const guint segid, GList *pts, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  // box for this track
  if(pts)
  {
    d->map.tracks->td[segid].map_box.lon1 = 180.0;
    d->map.tracks->td[segid].map_box.lon2 = -180.0;
    d->map.tracks->td[segid].map_box.lat1 = -90.0;
    d->map.tracks->td[segid].map_box.lat2 = 90.0;
    for(GList *pt = pts; pt; pt = g_list_next(pt))
    {
      dt_geo_map_display_point_t *p = pt->data;
      if(p->lon < d->map.tracks->td[segid].map_box.lon1)
        d->map.tracks->td[segid].map_box.lon1 = MAX(-180.0, p->lon);
      if(p->lon > d->map.tracks->td[segid].map_box.lon2)
        d->map.tracks->td[segid].map_box.lon2 = MIN(180.0, p->lon);
      if(p->lat > d->map.tracks->td[segid].map_box.lat1)
        d->map.tracks->td[segid].map_box.lat1 = MIN(90.0, p->lat);
      if(p->lat < d->map.tracks->td[segid].map_box.lat2)
        d->map.tracks->td[segid].map_box.lat2 = MAX(-90.0, p->lat);
    }
  }

  // box for all tracks
  float lon1 = 180.0;
  float lon2 = -180.0;
  float lat1 = -90.0;
  float lat2 = 90.0;
  for(int i = 0; i < d->map.nb_tracks; i++)
  {
    if(d->map.tracks->td[i].track)
    {
      if(d->map.tracks->td[i].map_box.lon1 < lon1)
        lon1 = d->map.tracks->td[i].map_box.lon1;
      if(d->map.tracks->td[i].map_box.lon2 > lon2)
        lon2 = d->map.tracks->td[i].map_box.lon2;
      if(d->map.tracks->td[i].map_box.lat1 > lat1)
        lat1 = d->map.tracks->td[i].map_box.lat1;
      if(d->map.tracks->td[i].map_box.lat2 < lat2)
        lat2 = d->map.tracks->td[i].map_box.lat2;
    }
  }
  const gboolean grow = lon1 < d->map.map_box.lon1 || lon2 > d->map.map_box.lon1 ||
                        lat1 > d->map.map_box.lat1 || lat2 < d->map.map_box.lat2;
  d->map.map_box.lon1 = lon1;
  d->map.map_box.lon2 = lon2;
  d->map.map_box.lat1 = lat1;
  d->map.map_box.lat2 = lat2;

  return grow;
}

static void _remove_tracks_from_map(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  if(d->map.tracks)
  {
    for(int i = 0; i < d->map.nb_tracks; i++)
    {
      if(d->map.tracks->td[i].track)
      {
        dt_view_map_remove_marker(darktable.view_manager, MAP_DISPLAY_TRACK,
                                  d->map.tracks->td[i].track);
        d->map.tracks->td[i].track = NULL;
      }
    }
    g_free(d->map.tracks);
    d->map.tracks = NULL;
  }
  if(d->map.gpx)
  {
    dt_gpx_destroy(d->map.gpx);
    d->map.gpx = NULL;
  }
}

GdkRGBA color[] = {(GdkRGBA){.red = 1.0, .green = 0.0, .blue = 0.0, .alpha = 0.5 },
                   (GdkRGBA){.red = 0.0, .green = 1.0, .blue = 1.0, .alpha = 0.5 },
                   (GdkRGBA){.red = 0.0, .green = 0.0, .blue = 1.0, .alpha = 0.5 },
                   (GdkRGBA){.red = 1.0, .green = 1.0, .blue = 0.0, .alpha = 0.5 },
                   (GdkRGBA){.red = 0.0, .green = 1.0, .blue = 0.0, .alpha = 0.5 },
                   (GdkRGBA){.red = 1.0, .green = 0.0, .blue = 1.0, .alpha = 0.5 }};

static gboolean _refresh_display_track(const gboolean active, const int segid, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  gboolean grow = FALSE;
  if(active)
  {
    GList *pts = dt_gpx_get_trkpts(d->map.gpx, segid);
    if(!d->map.tracks->td[segid].track)
      d->map.tracks->td[segid].track = dt_view_map_add_marker(darktable.view_manager,
                                                              MAP_DISPLAY_TRACK, pts);
    osm_gps_map_track_set_color((OsmGpsMapTrack *)d->map.tracks->td[segid].track, &color[segid % 6]);
    grow = _update_map_box(segid, pts, self);
    g_list_free_full(pts, g_free);
  }
  else
  {
    if(d->map.tracks->td[segid].track !=  NULL)
      dt_view_map_remove_marker(darktable.view_manager, MAP_DISPLAY_TRACK,
                                d->map.tracks->td[segid].track);
    d->map.tracks->td[segid].track =  NULL;
    _update_map_box(segid, NULL, self);
  }
  return grow;
}

static void _refresh_display_all_tracks(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->map.gpx_view));
  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
  gboolean grow = FALSE;
  for(int segid = 0; valid && segid < d->map.nb_tracks; segid++)
  {
    gboolean active;
    gtk_tree_model_get(model, &iter, DT_GEO_TRACKS_ACTIVE, &active, -1);
    grow = _refresh_display_track(active, segid, self) ? TRUE : grow;
    valid = gtk_tree_model_iter_next(model, &iter);
  }

  if(grow)
  {
    dt_view_map_center_on_bbox(darktable.view_manager, d->map.map_box.lon1, d->map.map_box.lat1,
                                                       d->map.map_box.lon2, d->map.map_box.lat2);
  }
  _refresh_displayed_images(self);
}

static void _track_seg_toggled(GtkCellRendererToggle *cell_renderer, gchar *path_str, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->map.gpx_view));
  GtkTreeIter iter;
  GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
  gboolean active;
  uint32_t segid;

  gtk_tree_model_get_iter(model, &iter, path);
  gtk_tree_model_get(model, &iter, DT_GEO_TRACKS_ACTIVE, &active,
                                   DT_GEO_TRACKS_SEGID, &segid, -1);
  gtk_list_store_set(GTK_LIST_STORE(model), &iter, DT_GEO_TRACKS_ACTIVE, !active, -1);
  gtk_tree_path_free(path);

  active = !active;
  if(_refresh_display_track(active, segid, self))
  {
    dt_view_map_center_on_bbox(darktable.view_manager, d->map.map_box.lon1, d->map.map_box.lat1,
                                                       d->map.map_box.lon2, d->map.map_box.lat2);
  }

  const gboolean preview = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->map.preview_button));
  _refresh_images_displayed_on_track(segid, active && preview, self);
  _update_nb_images(self);
  _update_buttons(self);
}

static void _all_tracks_toggled(GtkTreeViewColumn *column, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  GtkWidget *toggle = gtk_tree_view_column_get_widget(column);
  gboolean active = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), active);

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->map.gpx_view));
  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
  gboolean grow = FALSE;
  for(int segid = 0; valid && segid < d->map.nb_tracks; segid++)
  {
    gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                       DT_GEO_TRACKS_ACTIVE, active,-1);
    grow = _refresh_display_track(active, segid, self) ? TRUE : grow;
    valid = gtk_tree_model_iter_next(model, &iter);
  }
  if(active && grow)
  {
    dt_view_map_center_on_bbox(darktable.view_manager, d->map.map_box.lon1, d->map.map_box.lat1,
                                                       d->map.map_box.lon2, d->map.map_box.lat2);
  }
  _refresh_displayed_images(self);
  _update_nb_images(self);
  _update_buttons(self);
}

static void _select_images(GtkWidget *widget, dt_lib_module_t *self)
{
  GList *imgs = _get_images_on_active_tracks(self);
  dt_selection_clear(darktable.selection);
  dt_selection_select_list(darktable.selection, imgs);
  g_list_free(imgs);
}

static void _images_preview_toggled(GtkToggleButton *button, dt_lib_module_t *self)
{
  _refresh_displayed_images(self);
}

static void _refresh_track_list(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  if(!d->map.gpx) return;

  GList *trkseg = dt_gpx_get_trkseg(d->map.gpx);
  _remove_images_from_map(self);
  for(GList *i = d->imgs; i; i = g_list_next(i))
    ((dt_sel_img_t *)i->data)->segid = -1;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->map.gpx_view));
  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
  for(GList *ts = trkseg; ts && valid; ts = g_list_next(ts))
  {
    dt_gpx_track_segment_t *t = (dt_gpx_track_segment_t *)ts->data;
    gchar *dts = _utc_timeval_to_localtime_text(t->start_dt, d->tz_camera, TRUE);
    const int nb_imgs = _count_images_per_track(t, ts->next ? ts->next->data : NULL, self);
    gboolean active;
    gtk_tree_model_get(model, &iter, DT_GEO_TRACKS_ACTIVE, &active, -1);
    gchar *tooltip = _datetime_tooltip(t->start_dt, t->end_dt, d->tz_camera);
    gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                       DT_GEO_TRACKS_DATETIME, dts,
                       DT_GEO_TRACKS_POINTS, t->nb_trkpt,
                       DT_GEO_TRACKS_IMAGES, nb_imgs,
                       DT_GEO_TRACKS_TOOLTIP, tooltip,
                       -1);
    g_free(dts);
    g_free(tooltip);
    valid = gtk_tree_model_iter_next(model, &iter);
  }
  _update_nb_images(self);
  _refresh_displayed_images(self);
  _update_buttons(self);
}

static void _show_gpx_tracks(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  _remove_tracks_from_map(self);
  d->map.gpx = dt_gpx_new(gtk_label_get_text(GTK_LABEL(d->map.gpx_file)));
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->map.gpx_view));
  g_object_ref(model);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->map.gpx_view), NULL);
  gtk_list_store_clear(GTK_LIST_STORE(model));

  GList *trkseg = dt_gpx_get_trkseg(d->map.gpx);
  d->map.nb_tracks = g_list_length(trkseg);
  d->map.tracks = g_malloc0(sizeof(dt_lib_tracks_data_t) * d->map.nb_tracks);

  _remove_images_from_map(self);
  for(GList *i = d->imgs; i; i = g_list_next(i))
    ((dt_sel_img_t *)i->data)->segid = -1;

  int segid = 0;
  const gboolean active = gtk_toggle_button_get_active(
                          GTK_TOGGLE_BUTTON(gtk_tree_view_column_get_widget(d->map.sel_tracks)));
  GtkTreeIter iter;
  for(GList *ts = trkseg; ts; ts = g_list_next(ts))
  {
    dt_gpx_track_segment_t *t = (dt_gpx_track_segment_t *)ts->data;
    gchar *dts = _utc_timeval_to_localtime_text(t->start_dt, d->tz_camera, TRUE);
    gchar *tooltip = _datetime_tooltip(t->start_dt, t->end_dt, d->tz_camera);
    const int nb_imgs = _count_images_per_track(t, ts->next ? ts->next->data : NULL, self);
    gtk_list_store_append(GTK_LIST_STORE(model), &iter);
    gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                       DT_GEO_TRACKS_ACTIVE, active,
                       DT_GEO_TRACKS_DATETIME, dts,
                       DT_GEO_TRACKS_POINTS, t->nb_trkpt,
                       DT_GEO_TRACKS_IMAGES, nb_imgs,
                       DT_GEO_TRACKS_SEGID, segid,
                       DT_GEO_TRACKS_TOOLTIP, tooltip,
                       -1);
    segid++;
    g_free(dts);
    g_free(tooltip);
  }
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->map.gpx_view), model);
  g_object_unref(model);

  gtk_tree_view_column_set_clickable(d->map.sel_tracks, TRUE);
  _update_nb_images(self);
  _update_buttons(self);
  _refresh_display_all_tracks(self);
}

static void _apply_gpx(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  gchar *tz = dt_conf_get_string("plugins/lighttable/geotagging/tz");
  GList *imgs = _get_images_on_active_tracks(self);
  if(imgs)
  {
    if(d->offset)
    {
      GList *imgs2 = g_list_copy(imgs);
      dt_control_datetime(d->offset, NULL, imgs2);
    }
    dt_control_gpx_apply(gtk_label_get_text(GTK_LABEL(d->map.gpx_file)), -1, tz, imgs);
  }
  g_free(tz);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->map.preview_button), FALSE);
}

static void _update_layout(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  gtk_widget_set_visible(d->gpx_button, !d->map.view);
  gtk_widget_set_visible(d->map.gpx_section, d->map.view);
}

static void _view_changed(gpointer instance, dt_view_t *old_view,
                          dt_view_t *new_view, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  if(old_view != new_view)
  {
    d->map.view = !g_strcmp0(new_view->module_name, "map");
    if(d->map.view)
    {
      _setup_selected_images_list(self);
      _refresh_track_list(self);
    }
    _update_layout(self);
  }
}

static void _geotag_changed(gpointer instance, GList *imgs, const int locid, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  if(d->map.view && !locid)
  {
    _refresh_displayed_images(self);
    _update_nb_images(self);
    _update_buttons(self);
  }
}

static void _refresh_selected_images_datetime(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  for(GList *i = d->imgs; i; i = g_list_next(i))
  {
    dt_sel_img_t *img = i->data;
    const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, img->imgid, 'r');
    if(!cimg) continue;
    dt_datetime_img_to_exif(img->dt, sizeof(img->dt), cimg);
    dt_image_cache_read_release(darktable.image_cache, cimg);
  }
}

static gboolean _row_tooltip_setup(GtkWidget *view, gint x, gint y, gboolean kb_mode,
                                   GtkTooltip* tooltip, dt_lib_module_t *self)
{
  gboolean res = FALSE;
  GtkTreePath *path = NULL;
  GtkTreeViewColumn *column = NULL;
  // Get view path mouse position
  gint tx, ty;
  gtk_tree_view_convert_widget_to_bin_window_coords(GTK_TREE_VIEW(view), x, y, &tx, &ty);
  if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), tx, ty, &path, &column, NULL, NULL))
  {
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, path))
    {
      char *text = NULL;
      gtk_tree_model_get(model, &iter, DT_GEO_TRACKS_TOOLTIP, &text, -1);
      if(text && text[0] != '\0')
      {
        gtk_tooltip_set_text(tooltip, text);
        res = TRUE;
      }
      g_free(text);
    }
  }
  gtk_tree_path_free(path);
  return res;
}
#endif // HAVE_MAP

static void _preview_gpx_file(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
            _("GPX file track segments"), GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
            _("done"), GTK_RESPONSE_CANCEL, NULL);

  gchar *filedir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(widget));
  struct dt_gpx_t *gpx = dt_gpx_new(filedir);
  g_free(filedir);

  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *w = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_size_request(w, -1, DT_PIXEL_APPLY_DPI(100));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_NEVER, GTK_POLICY_NEVER);
  gtk_box_pack_start(GTK_BOX(area), w, TRUE, TRUE, 0);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(10));
  int line = 0;

  GList *trkseg = dt_gpx_get_trkseg(gpx);

  _set_up_label(_("name"), GTK_ALIGN_START, grid, 0, line, PANGO_ELLIPSIZE_NONE);
  _set_up_label(_("start time"), GTK_ALIGN_START, grid, 1, line, PANGO_ELLIPSIZE_NONE);
  _set_up_label(_("end time"), GTK_ALIGN_START, grid, 2, line, PANGO_ELLIPSIZE_NONE);
  _set_up_label(_("points"), GTK_ALIGN_CENTER, grid, 3, line, PANGO_ELLIPSIZE_NONE);
  _set_up_label(_("images"), GTK_ALIGN_CENTER, grid, 4, line, PANGO_ELLIPSIZE_NONE);

  for(GList *i = d->imgs; i; i = g_list_next(i))
    ((dt_sel_img_t *)i->data)->segid = -1;
  int total_imgs = 0;
  int total_pts = 0;
  line++;
  for(GList *ts = trkseg; ts; ts = g_list_next(ts))
  {
    dt_gpx_track_segment_t *t = (dt_gpx_track_segment_t *)ts->data;
    gchar *dts = _utc_timeval_to_localtime_text(t->start_dt, d->tz_camera, TRUE);
    gchar *dte = _utc_timeval_to_localtime_text(t->end_dt, d->tz_camera, TRUE);

    const int nb_imgs = _count_images_per_track(t, ts->next ? ts->next->data : NULL,self);
    total_imgs += nb_imgs;

    _set_up_label(t->name, GTK_ALIGN_START, grid, 0, line, PANGO_ELLIPSIZE_NONE);
    _set_up_label(dts, GTK_ALIGN_START, grid, 1, line, PANGO_ELLIPSIZE_NONE);
    _set_up_label(dte, GTK_ALIGN_START, grid, 2, line, PANGO_ELLIPSIZE_NONE);
    char *nb = g_strdup_printf("%u", t->nb_trkpt);
    _set_up_label(nb, GTK_ALIGN_CENTER, grid, 3, line, PANGO_ELLIPSIZE_NONE);
    g_free(nb);
    nb = g_strdup_printf("%d", nb_imgs);
    _set_up_label(nb, GTK_ALIGN_CENTER, grid, 4, line, PANGO_ELLIPSIZE_NONE);
    g_free(nb);
    line++;
    total_pts += t->nb_trkpt;
    g_free(dts);
    g_free(dte);
  }

  char *nb = g_strdup_printf("%d", total_pts);
  _set_up_label(nb, GTK_ALIGN_CENTER, grid, 3, line, PANGO_ELLIPSIZE_NONE);
  g_free(nb);
  nb = g_strdup_printf("%d / %d", total_imgs, d->nb_imgs);
  _set_up_label(nb, GTK_ALIGN_CENTER, grid, 4, line, PANGO_ELLIPSIZE_NONE);
  g_free(nb);

  dt_gpx_destroy(gpx);

  gtk_container_add(GTK_CONTAINER(w), grid);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);
  gtk_dialog_run(GTK_DIALOG(dialog));

  gtk_widget_destroy(dialog);
}

static void _setup_selected_images_list(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  if(d->imgs)
  {
#ifdef HAVE_MAP
    _remove_images_from_map(self);
#endif
    g_list_free_full(d->imgs, g_free);
  }
  d->imgs = NULL;
  d->nb_imgs = 0;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid FROM main.selected_images",
                              -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const uint32_t imgid = sqlite3_column_int(stmt, 0);
    const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    char dt[DT_DATETIME_LENGTH];
    if(!cimg) continue;
    dt_datetime_img_to_exif(dt, sizeof(dt), cimg);
    dt_image_cache_read_release(darktable.image_cache, cimg);

    dt_sel_img_t *img = g_malloc0(sizeof(dt_sel_img_t));
    if(!img) continue;
    memcpy(img->dt, dt, DT_DATETIME_LENGTH);
    img->imgid = imgid;
    d->imgs = g_list_prepend(d->imgs, img);
    d->nb_imgs++;
  }
  sqlite3_finalize(stmt);
}

static void _choose_gpx_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  // bring a filechooser to select the gpx file to apply to selection
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
            _("open GPX file"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN,
            _("preview"), GTK_RESPONSE_ACCEPT,
            _("_cancel"), GTK_RESPONSE_CANCEL,
            _("_open"), GTK_RESPONSE_OK, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif

  dt_conf_get_folder_to_file_chooser("ui_last/gpx_last_directory", GTK_FILE_CHOOSER(filechooser));

  GtkFileFilter *filter;
  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_custom(filter, GTK_FILE_FILTER_FILENAME | GTK_FILE_FILTER_MIME_TYPE,
                             _lib_geotagging_filter_gpx, NULL, NULL);
  gtk_file_filter_set_name(filter, _("GPS data exchange format"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if(!d->imgs)
    _setup_selected_images_list(self);

  int res = gtk_dialog_run(GTK_DIALOG(filechooser));
  while(res == GTK_RESPONSE_ACCEPT)
  {
    _preview_gpx_file(filechooser, self);
    res = gtk_dialog_run(GTK_DIALOG(filechooser));
  }
  if(res == GTK_RESPONSE_OK)
  {
    dt_conf_set_folder_from_file_chooser("ui_last/gpx_last_directory", GTK_FILE_CHOOSER(filechooser));

    gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));

#ifdef HAVE_MAP
    if(d->map.view)
    {
      gtk_label_set_text(GTK_LABEL(d->map.gpx_file), filename);
      _show_gpx_tracks(self);
      gtk_widget_set_visible(d->map.gpx_view, d->map.view);
    }
    else
#endif
    {
      gchar *tz = dt_conf_get_string("plugins/lighttable/geotagging/tz");
      dt_control_gpx_apply(filename, -1, tz, NULL);
      g_free(tz);
      g_list_free_full(d->imgs, g_free);
      d->imgs = NULL;
      d->nb_imgs = 0;
    }
    g_free(filename);
  }

  gtk_widget_destroy(filechooser);
  //   dt_control_queue_redraw_center();
}

static int _sort_timezones(gconstpointer a, gconstpointer b)
{
  const tz_tuple_t *tz_a = (tz_tuple_t *)a;
  const tz_tuple_t *tz_b = (tz_tuple_t *)b;

#ifdef _WIN32
  gboolean utc_neg_a = g_str_has_prefix(tz_a->display, "(UTC-");
  gboolean utc_neg_b = g_str_has_prefix(tz_b->display, "(UTC-");

  gboolean utc_pos_a = g_str_has_prefix(tz_a->display, "(UTC+");
  gboolean utc_pos_b = g_str_has_prefix(tz_b->display, "(UTC+");

  if(utc_neg_a && utc_neg_b)
  {
    char *iter_a = tz_a->display + strlen("(UTC-");
    char *iter_b = tz_b->display + strlen("(UTC-");

    while(((*iter_a >= '0' && *iter_a <= '9') || *iter_a == ':') &&
          ((*iter_b >= '0' && *iter_b <= '9') || *iter_b == ':'))
    {
      if(*iter_a != *iter_b) return *iter_b - *iter_a;
      iter_a++;
      iter_b++;
    }
  }
  else if(utc_neg_a && utc_pos_b) return -1;
  else if(utc_pos_a && utc_neg_b) return 1;
#endif

  return g_strcmp0(tz_a->display, tz_b->display);
}

// create a list of possible time zones
static GList *_lib_geotagging_get_timezones(void)
{
  GList *timezones = NULL;

#ifndef _WIN32
  // possible locations for zone.tab:
  // - /usr/share/zoneinfo
  // - /usr/lib/zoneinfo
  // - getenv("TZDIR")
  // - apparently on solaris there is no zones.tab. we need to collect the information ourselves like this:
  //   /bin/grep -h ^Zone /usr/share/lib/zoneinfo/src/* | /bin/awk '{print "??\t+9999+99999\t" $2}'
#define MAX_LINE_LENGTH 256
  FILE *fp;
  char line[MAX_LINE_LENGTH];

  // find the file using known possible locations
  gchar *zone_tab = g_strdup("/usr/share/zoneinfo/zone.tab");
  if(!g_file_test(zone_tab, G_FILE_TEST_IS_REGULAR))
  {
    g_free(zone_tab);
    zone_tab = g_strdup("/usr/lib/zoneinfo/zone.tab");
    if(!g_file_test(zone_tab, G_FILE_TEST_IS_REGULAR))
    {
      g_free(zone_tab);
      zone_tab = g_build_filename(g_getenv("TZDIR"), "zone.tab", NULL);
      if(!g_file_test(zone_tab, G_FILE_TEST_IS_REGULAR))
      {
        g_free(zone_tab);
        char datadir[PATH_MAX] = { 0 };
        dt_loc_get_datadir(datadir, sizeof(datadir));
        zone_tab = g_build_filename(datadir, "zone.tab", NULL);
        if(!g_file_test(zone_tab, G_FILE_TEST_IS_REGULAR))
        {
          g_free(zone_tab);
          // TODO: Solaris test
          return NULL;
        }
      }
    }
  }

  // parse zone.tab and put all time zone descriptions into timezones
  fp = g_fopen(zone_tab, "r");
  g_free(zone_tab);

  if(!fp) return NULL;

  while(fgets(line, MAX_LINE_LENGTH, fp))
  {
    if(line[0] == '#' || line[0] == '\0') continue;
    gchar **tokens = g_strsplit_set(line, " \t\n", 0);
    // sometimes files are not separated by single tabs but multiple spaces, resulting in empty strings in tokens
    // so we have to look for the 3rd non-empty entry
    int n_found = -1, i;
    for(i = 0; tokens[i] && n_found < 2; i++) if(*tokens[i]) n_found++;
    if(n_found != 2)
    {
      g_strfreev(tokens);
      continue;
    }
    gchar *name = g_strdup(tokens[i - 1]);
    g_strfreev(tokens);
    if(name[0] == '\0')
    {
      g_free(name);
      continue;
    }
    size_t last_char = strlen(name) - 1;
    if(name[last_char] == '\n') name[last_char] = '\0';
    tz_tuple_t *tz_tuple = (tz_tuple_t *)malloc(sizeof(tz_tuple_t));
    tz_tuple->display = name;
    tz_tuple->name = name;
    timezones = g_list_prepend(timezones, tz_tuple);
  }

  fclose(fp);

  // sort timezones
  timezones = g_list_sort(timezones, _sort_timezones);

  tz_tuple_t *utc = (tz_tuple_t *)malloc(sizeof(tz_tuple_t));
  utc->display = g_strdup("UTC");
  utc->name = utc->display;
  timezones = g_list_prepend(timezones, utc);

#undef MAX_LINE_LENGTH

#else // !_WIN32
  // on Windows we have to grab the time zones from the registry
  char *keypath = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\";
  HKEY hKey;

  if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                   keypath,
                   0,
                   KEY_READ,
                   &hKey) == ERROR_SUCCESS)
  {
    DWORD n_subkeys, max_subkey_len;

    if(RegQueryInfoKey(hKey,
                       NULL,
                       NULL,
                       NULL,
                       &n_subkeys,
                       &max_subkey_len,
                       NULL,
                       NULL,
                       NULL,
                       NULL,
                       NULL,
                       NULL) == ERROR_SUCCESS)
    {
      wchar_t *subkeyname = (wchar_t *)malloc(sizeof(wchar_t) * (max_subkey_len + 1));

      for(DWORD i = 1; i < n_subkeys; i++)
      {
        DWORD subkeyname_length = max_subkey_len + 1;
        if(RegEnumKeyExW(hKey,
                         i,
                         subkeyname,
                         &subkeyname_length,
                         NULL,
                         NULL,
                         NULL,
                         NULL) == ERROR_SUCCESS)
        {
          DWORD buffer_size;
          char *subkeyname_utf8 = g_utf16_to_utf8(subkeyname, -1, NULL, NULL, NULL);
          char *subkeypath_utf8 = g_strconcat(keypath, "\\", subkeyname_utf8, NULL);
          wchar_t *subkeypath = g_utf8_to_utf16(subkeypath_utf8, -1, NULL, NULL, NULL);
          if(RegGetValueW(HKEY_LOCAL_MACHINE,
                          subkeypath,
                          L"Display",
                          RRF_RT_ANY,
                          NULL,
                          NULL,
                          &buffer_size) == ERROR_SUCCESS)
          {
            wchar_t *display_name = (wchar_t *)malloc(buffer_size);
            if(RegGetValueW(HKEY_LOCAL_MACHINE,
                            subkeypath,
                            L"Display",
                            RRF_RT_ANY,
                            NULL,
                            display_name,
                            &buffer_size) == ERROR_SUCCESS)
            {
              tz_tuple_t *tz = (tz_tuple_t *)malloc(sizeof(tz_tuple_t));

              tz->name = subkeyname_utf8;
              tz->display = g_utf16_to_utf8(display_name, -1, NULL, NULL, NULL);
              timezones = g_list_prepend(timezones, tz);

              subkeyname_utf8 = NULL; // to not free it later
            }
            free(display_name);
          }
          g_free(subkeyname_utf8);
          g_free(subkeypath_utf8);
          g_free(subkeypath);
        }
      }

      free(subkeyname);
    }
  }

  RegCloseKey(hKey);

  timezones = g_list_sort(timezones, _sort_timezones);
#endif // !_WIN32

  return timezones;
}

static void _display_offset(const GTimeSpan offset_int, const gboolean valid, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  GTimeSpan off2 = 0;
  if(valid)
  {
    const gboolean neg = offset_int < 0;
    gtk_label_set_text(GTK_LABEL(d->of.sign), neg ? "- " : "");
    char text[4];
    GTimeSpan off = neg ? -offset_int : offset_int;
    off2 = off / 1000;  // skip microseconds
    off = off2;
    off2 = off / 1000;
    snprintf(text, sizeof(text), "%03d", (int)(off - off2 * 1000));
    gtk_entry_set_text(GTK_ENTRY(d->of.widget[6]), text);
    off = off2;
    off2 = off / 60;
    snprintf(text, sizeof(text), "%02d", (int)(off - off2 * 60));
    gtk_entry_set_text(GTK_ENTRY(d->of.widget[5]), text);
    off = off2;
    off2 = off / 60;
    snprintf(text, sizeof(text), "%02d", (int)(off - off2 * 60));
    gtk_entry_set_text(GTK_ENTRY(d->of.widget[4]), text);
    off = off2;
    off2 = off / 24;
    snprintf(text, sizeof(text), "%02d", (int)(off - off2 * 24));
    gtk_entry_set_text(GTK_ENTRY(d->of.widget[3]), text);
    off = off2;
    off2 = off / 100;
    snprintf(text, sizeof(text), "%02d", (int)(off - off2 * 100));
    gtk_entry_set_text(GTK_ENTRY(d->of.widget[2]), text);
  }
  if(!valid || off2)
  {
    gtk_label_set_text(GTK_LABEL(d->of.sign), "");
    for(int i = 2; i < DT_GEOTAG_PARTS_NB; i++)
      gtk_entry_set_text(GTK_ENTRY(d->of.widget[i]), "-");
  }
  const gboolean locked = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->lock_offset));
  gtk_widget_set_sensitive(d->apply_offset, d->imgid && valid && !off2 && offset_int);
  gtk_widget_set_sensitive(d->lock_offset, locked || (d->imgid && valid && !off2 && offset_int));
  gtk_widget_set_sensitive(d->apply_datetime, d->imgid && !locked);
#ifdef HAVE_MAP
  _update_buttons(self);
#endif
}

static void _display_datetime(dt_lib_datetime_t *dtw, GDateTime *datetime,
                              const gboolean lock, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  for(int i = 0; lock && i < DT_GEOTAG_PARTS_NB; i++)
    g_signal_handlers_block_by_func(d->dt.widget[i], _datetime_entry_changed, self);
  if(datetime)
  {
    char value[8] = {0};
    snprintf(value, sizeof(value), "%04d", g_date_time_get_year(datetime));
    gtk_entry_set_text(GTK_ENTRY(dtw->widget[0]), value);
    snprintf(value, sizeof(value), "%02d", g_date_time_get_month(datetime));
    gtk_entry_set_text(GTK_ENTRY(dtw->widget[1]), value);
    snprintf(value, sizeof(value), "%02d", g_date_time_get_day_of_month(datetime));
    gtk_entry_set_text(GTK_ENTRY(dtw->widget[2]), value);
    snprintf(value, sizeof(value), "%02d", g_date_time_get_hour(datetime));
    gtk_entry_set_text(GTK_ENTRY(dtw->widget[3]), value);
    snprintf(value, sizeof(value), "%02d", g_date_time_get_minute(datetime));
    gtk_entry_set_text(GTK_ENTRY(dtw->widget[4]),value);
    snprintf(value, sizeof(value), "%02d", g_date_time_get_second(datetime));
    gtk_entry_set_text(GTK_ENTRY(dtw->widget[5]), value);
    snprintf(value, sizeof(value), "%03d", (int)(g_date_time_get_microsecond(datetime) * 0.001));
    gtk_entry_set_text(GTK_ENTRY(dtw->widget[6]), value);
  }
  else
  {
    for(int i = 0; i < DT_GEOTAG_PARTS_NB; i++)
      gtk_entry_set_text(GTK_ENTRY(dtw->widget[i]), "-");
  }
  for(int i = 0; lock && i < DT_GEOTAG_PARTS_NB; i++)
    g_signal_handlers_unblock_by_func(d->dt.widget[i], _datetime_entry_changed, self);
}

// read the current date/time and make correction (field under/overflow)
static GDateTime *_read_datetime_entry(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;

  const int year = atoi(gtk_entry_get_text(GTK_ENTRY(d->dt.widget[0])));
  const int month = atoi(gtk_entry_get_text(GTK_ENTRY(d->dt.widget[1])));
  const int day = atoi(gtk_entry_get_text(GTK_ENTRY(d->dt.widget[2])));
  const int hour = atoi(gtk_entry_get_text(GTK_ENTRY(d->dt.widget[3])));
  const int minute = atoi(gtk_entry_get_text(GTK_ENTRY(d->dt.widget[4])));
  const int second = atoi(gtk_entry_get_text(GTK_ENTRY(d->dt.widget[5])));
  const int millisecond = atoi(gtk_entry_get_text(GTK_ENTRY(d->dt.widget[6])));
  const gdouble second2 = (gdouble)second + (gdouble)millisecond * 0.001;

  return g_date_time_new(darktable.utc_tz, year, month, day, hour, minute, second2);
}

static void _new_datetime(GDateTime *datetime, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  if(datetime)
  {
    _display_datetime(&d->dt, datetime, TRUE, self);

    if(d->datetime)
      g_date_time_unref(d->datetime);
    d->datetime = datetime;
    d->offset = g_date_time_difference(d->datetime, d->datetime0);
    _display_offset(d->offset, d->datetime != NULL, self);
#ifdef HAVE_MAP
    if(d->map.view)
      _refresh_track_list(self);
#endif
  }
}

static void _datetime_entry_changed(GtkWidget *entry, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  if(!d->editing)
  {
    GDateTime *datetime = _read_datetime_entry(self);
    _new_datetime(datetime, self);
  }
}

static GDateTime *_get_image_datetime(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  GList *selected = dt_collection_get_selected(darktable.collection, 1);
  const int selid = selected ? GPOINTER_TO_INT(selected->data) : 0;
  const int imgid = dt_act_on_get_main_image();
  GDateTime *datetime = NULL;
  if((selid != 0) || ((selid == 0) && (imgid != -1)))
  {
    // consider act on only if no selected
    char datetime_s[DT_DATETIME_LENGTH];
    dt_image_get_datetime(selid ? selid : imgid, datetime_s);
    if(datetime_s[0] != '\0')
      datetime = dt_datetime_exif_to_gdatetime(datetime_s, darktable.utc_tz);
    else
      datetime = NULL;
  }
  d->imgid = selid;
  return datetime;
}

static void _refresh_image_datetime(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  const gboolean locked = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->lock_offset));
  GDateTime *datetime = _get_image_datetime(self);
  if(d->datetime0)
    g_date_time_unref(d->datetime0);
  d->datetime0 = datetime;
  _display_datetime(&d->dt0, datetime, FALSE, self);
  if(locked)
  {
    GDateTime *datetime2 = g_date_time_add(datetime, d->offset);
    _new_datetime(datetime2, self);
  }
  else
  {
    _display_offset(d->offset = 0, datetime != NULL, self);
    if(datetime)
    {
      g_date_time_ref(datetime);
      _new_datetime(datetime, self);
    }
  }
}

static void _image_info_changed(gpointer instance, gpointer imgs, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  for(GList *i = imgs; i; i = g_list_next(i))
  {
    if(GPOINTER_TO_INT(i->data) == d->imgid)
    {
      _refresh_image_datetime(self);
      break;
    }
  }
#ifdef HAVE_MAP
  if(d->map.view)
  {
    _refresh_selected_images_datetime(self);
    _refresh_track_list(self);
  }
#endif
}

static void _mouse_over_image_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  if(!d->imgid)
    _refresh_image_datetime(self);
}

static void _selection_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  _refresh_image_datetime(self);
#ifdef HAVE_MAP
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  if(d->map.view)
  {
    _setup_selected_images_list(self);
    _refresh_track_list(self);
  }
#endif
}

static gboolean _datetime_scroll_over(GtkWidget *w, GdkEventScroll *event, dt_lib_module_t *self)
{
  if(dt_gui_ignore_scroll(event)) return FALSE;

  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  if(!d->editing)
  {
    int i = 0;
    for(i = 0; i < DT_GEOTAG_PARTS_NB; i++)
      if(w == d->dt.widget[i]) break;

    int delta_y;
    int increment = 0;
    if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
    {
      if(delta_y < 0) increment = 1;
      else if(delta_y > 0) increment = -1;
    }

    if(dt_modifier_is(event->state, GDK_SHIFT_MASK))
      increment *= 10;

    GDateTime *datetime;
    switch(i)
    {
      case 0:
        datetime = g_date_time_add_years(d->datetime, increment);
        break;
      case 1:
        datetime = g_date_time_add_months(d->datetime, increment);
        break;
      case 2:
        datetime = g_date_time_add_days(d->datetime, increment);
        break;
      case 3:
        datetime = g_date_time_add_hours(d->datetime, increment);
        break;
      case 4:
        datetime = g_date_time_add_minutes(d->datetime, increment);
        break;
      case 5:
        datetime = g_date_time_add_seconds(d->datetime, increment);
        break;
      case 6:
        datetime = g_date_time_add(d->datetime, increment * 1000);
        break;
      default:
        datetime = NULL;
    }

    _new_datetime(datetime, self);
  }

  return TRUE;
}

// type 0 date/time, 1 original date/time, 2 offset
static GtkWidget *_gui_init_datetime(gchar *text, dt_lib_datetime_t *dt, const int type, dt_lib_module_t *self,
                                    GtkSizeGroup *group, GtkWidget *button, gchar *tip)
{
  GtkWidget *flow = gtk_flow_box_new();
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 2);

  GtkWidget *t = dt_ui_label_new(text);
  gtk_size_group_add_widget(group, t);
  gtk_container_add(GTK_CONTAINER(flow), t);
  gtk_widget_set_tooltip_text(flow, tip);

  GtkWidget *flow2 = gtk_flow_box_new();
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow2), TRUE); // otherwise weird behavior
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow2), 2);
  gtk_container_add(GTK_CONTAINER(flow), flow2);

  GtkBox *box = NULL;
  for(int i = 0; i < DT_GEOTAG_PARTS_NB; i++)
  {
    if(!box) box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));

    if(i == 0 && type == 2)
    {
      gtk_box_set_homogeneous(box, TRUE); // creates some spacing
      gtk_box_pack_start(box, button, TRUE, TRUE, 0);
      dt->sign = gtk_label_new("");
      gtk_box_pack_start(box, dt->sign, FALSE, FALSE, 0);
    }
    if(i >= 2 || type != 2)
    {
      dt->widget[i] = gtk_entry_new();
      gtk_entry_set_width_chars(GTK_ENTRY(dt->widget[i]), i == 0 ? 4 : i == 6 ? 3 : 2);
      gtk_entry_set_alignment(GTK_ENTRY(dt->widget[i]), 0.5);
      gtk_box_pack_start(box, dt->widget[i], FALSE, FALSE, 0);
      if(type == 0)
      {
        dt_action_define(DT_ACTION(self), NULL, i <= 2 ? N_("date") : N_("time"), dt->widget[i], &dt_action_def_entry);
        gtk_widget_add_events(dt->widget[i], darktable.gui->scroll_mask);
      }
      else
      {
        gtk_widget_set_sensitive(dt->widget[i], FALSE);
      }
    }

    if(i == 2 || i == 6)
    {
      gtk_widget_set_halign(GTK_WIDGET(box), GTK_ALIGN_END);
      gtk_widget_set_hexpand(GTK_WIDGET(box), TRUE);
      gtk_container_add(GTK_CONTAINER(flow2), GTK_WIDGET(box));
      box = NULL;
    }
    else if(i > 2 || type != 2)
    {
      GtkWidget *label = gtk_label_new(i < 2 ? "-" : i == 5 ? "." :":");
      if(i == 5)
        g_object_set_data(G_OBJECT(dt->widget[i]), "msec_label", label);
      gtk_box_pack_start(box, label, FALSE, FALSE, 0);
    }
  }

  gtk_container_foreach(GTK_CONTAINER(flow2), (GtkCallback)gtk_widget_set_can_focus, GINT_TO_POINTER(FALSE));

  return flow;
}

static gboolean _datetime_key_pressed(GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  switch(event->keyval)
  {
    case GDK_KEY_Escape:
    {
      // reset
      _refresh_image_datetime(self);
#ifdef HAVE_MAP
      if(d->map.view)
        _refresh_track_list(self);
#endif
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      d->editing = FALSE;
      return FALSE;
    }
    // allow  0 .. 9, left/right/home/end movement using arrow keys and del/backspace
    case GDK_KEY_0:
    case GDK_KEY_KP_0:
    case GDK_KEY_1:
    case GDK_KEY_KP_1:
    case GDK_KEY_2:
    case GDK_KEY_KP_2:
    case GDK_KEY_3:
    case GDK_KEY_KP_3:
    case GDK_KEY_4:
    case GDK_KEY_KP_4:
    case GDK_KEY_5:
    case GDK_KEY_KP_5:
    case GDK_KEY_6:
    case GDK_KEY_KP_6:
    case GDK_KEY_7:
    case GDK_KEY_KP_7:
    case GDK_KEY_8:
    case GDK_KEY_KP_8:
    case GDK_KEY_9:
    case GDK_KEY_KP_9:
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:
    case GDK_KEY_BackSpace:
    case GDK_KEY_Left:
    case GDK_KEY_Right:
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:
    case GDK_KEY_End:
    case GDK_KEY_KP_End:
      d->editing = TRUE;
      return FALSE;

    case GDK_KEY_Tab:
    case GDK_KEY_KP_Tab:
    case GDK_KEY_ISO_Left_Tab:
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      d->editing = FALSE;
      g_signal_emit_by_name(d->dt.widget[0], "changed");
      return FALSE;

    default: // block everything else
      return TRUE;
  }
}

static void _timezone_save(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  const gchar *tz = gtk_entry_get_text(GTK_ENTRY(d->timezone));

  gchar *name = NULL;
  for(GList *iter = d->timezones; iter; iter = g_list_next(iter))
  {
    tz_tuple_t *tz_tuple = (tz_tuple_t *)iter->data;
    if(!strcmp(tz_tuple->display, tz))
      name = tz_tuple->name;
  }
  if(d->tz_camera) g_time_zone_unref(d->tz_camera);
  d->tz_camera = !name ? g_time_zone_new_utc() : g_time_zone_new(name);
  dt_conf_set_string("plugins/lighttable/geotagging/tz", name ? name : "UTC");
  gtk_entry_set_text(GTK_ENTRY(d->timezone), name ? name : "UTC");
  gtk_label_set_text(GTK_LABEL (d->timezone_changed), "");

  gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
#ifdef HAVE_MAP
  if(d->map.view)
    _refresh_track_list(self);
#endif
}

static gboolean _timezone_key_pressed(GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self)
{
  switch(event->keyval)
  {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_Tab:
      _timezone_save(self);
      return TRUE;
    case GDK_KEY_Escape:
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      return TRUE;
    default: ;
      dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
      gtk_label_set_text(GTK_LABEL (d->timezone_changed), " *");
      break;
  }
  return FALSE;
}

static gboolean _timezone_focus_out(GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self)
{
  _timezone_save(self);
  return FALSE;
}

static gboolean _completion_match_func(GtkEntryCompletion *completion, const gchar *key, GtkTreeIter *iter,
                                       gpointer user_data)
{
  gboolean res = FALSE;

  GtkEditable *e = (GtkEditable *)gtk_entry_completion_get_entry(completion);
  if(!GTK_IS_EDITABLE(e))
    return FALSE;

  GtkTreeModel *model = gtk_entry_completion_get_model(completion);
  const int column = gtk_entry_completion_get_text_column(completion);

  if(gtk_tree_model_get_column_type(model, column) != G_TYPE_STRING)
    return FALSE;

  char *tag = NULL;
  gtk_tree_model_get(model, iter, column, &tag, -1);
  if(tag)
  {
    char *normalized = g_utf8_normalize(tag, -1, G_NORMALIZE_ALL);
    if(normalized)
    {
      char *casefold = g_utf8_casefold(normalized, -1);
      if(casefold)
      {
        res = g_strstr_len(casefold, -1, key) != NULL;
      }
      g_free(casefold);
    }
    g_free(normalized);
    g_free(tag);
  }

  return res;
}

static void _toggle_lock_button_callback(GtkToggleButton *button, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  const gboolean locked = gtk_toggle_button_get_active(button);
  for(int i = 0; i < DT_GEOTAG_PARTS_NB; i++)
  {
    gtk_widget_set_sensitive(d->dt.widget[i], !locked);
  }
  gtk_widget_set_sensitive(d->apply_datetime, d->imgid && !locked);
}

GtkTreeViewColumn *_new_tree_text_column(const char *name, const gboolean expand,
                                         const float xalign, const int m_col,
                                         const int ellipsize)
{
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *column = gtk_tree_view_column_new();
  gtk_tree_view_column_pack_start(column, renderer, TRUE);
  gtk_tree_view_column_set_attributes(column, renderer, "text", m_col, NULL);
  gtk_tree_view_column_set_expand(column, expand);
  GtkWidget *label = gtk_label_new(name);
  gtk_widget_show(label);
  gtk_tree_view_column_set_widget(column, label);
  gtk_label_set_ellipsize(GTK_LABEL(label), expand ? PANGO_ELLIPSIZE_MIDDLE : PANGO_ELLIPSIZE_NONE);
  g_object_set(renderer, "xalign", xalign, "ellipsize", ellipsize, NULL);
  return column;
}

static void _show_milliseconds(dt_lib_geotagging_t *d)
{
  const gboolean milliseconds = dt_conf_get_bool("lighttable/ui/milliseconds");
  gtk_widget_set_visible(d->dt.widget[6], milliseconds);
  gtk_widget_set_visible(d->dt0.widget[6], milliseconds);
  gtk_widget_set_visible(d->of.widget[6], milliseconds);
  gtk_widget_set_visible(g_object_get_data(G_OBJECT(d->dt.widget[5]), "msec_label"), milliseconds);
  gtk_widget_set_visible(g_object_get_data(G_OBJECT(d->dt0.widget[5]), "msec_label"), milliseconds);
  gtk_widget_set_visible(g_object_get_data(G_OBJECT(d->of.widget[5]), "msec_label"), milliseconds);
}

static void _dt_pref_change_callback(gpointer instance, dt_lib_module_t *self)
{
  _show_milliseconds((dt_lib_geotagging_t *)self->data);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)g_malloc0(sizeof(dt_lib_geotagging_t));
  self->data = (void *)d;
  d->timezones = _lib_geotagging_get_timezones();
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkGrid *grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  int line = 0;

  GtkSizeGroup *group = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
  GtkWidget *box = _gui_init_datetime(_("date/time"), &d->dt, 0, self, group, NULL,
                                      _("enter the new date/time (YYYY:MM:DD hh:mm:ss[.sss])"
                                        "\nkey in the new numbers or scroll over the cell"));
  gtk_grid_attach(grid, box, 0, line++, 4, 1);

  box = _gui_init_datetime(_("original date/time"), &d->dt0, 1, self, group, NULL, NULL);
  gtk_grid_attach(grid, box, 0, line++, 4, 1);

  d->lock_offset = dtgtk_togglebutton_new(dtgtk_cairo_paint_lock, 0, NULL);
  gtk_widget_set_tooltip_text(d->lock_offset, _("lock date/time offset value to apply it onto another selection"));
  gtk_widget_set_halign(d->lock_offset, GTK_ALIGN_START);
  g_signal_connect(G_OBJECT(d->lock_offset), "clicked", G_CALLBACK(_toggle_lock_button_callback), (gpointer)self);

  box = _gui_init_datetime(_("date/time offset"), &d->of, 2, self, group, d->lock_offset,
                           _("offset or difference ([-]dd hh:mm:ss[.sss])"));
  gtk_grid_attach(grid, box, 0, line++, 4, 1);

  // apply
  d->apply_offset = dt_action_button_new(self, N_("apply offset"), _apply_offset_callback, self,
                                         _("apply offset to selected images"), 0, 0);
  gtk_grid_attach(grid, d->apply_offset , 0, line, 2, 1);

  d->apply_datetime = dt_action_button_new(self, N_("apply date/time"), _apply_datetime_callback, self,
                                           _("apply the same date/time to selected images"), 0, 0);
  gtk_grid_attach(grid, d->apply_datetime , 2, line++, 2, 1);

  // time zone entry
  GtkWidget *label = dt_ui_label_new(_(dt_confgen_get_label("plugins/lighttable/geotagging/tz")));
  gtk_widget_set_tooltip_text(label, _(dt_confgen_get_tooltip("plugins/lighttable/geotagging/tz")));

  gtk_grid_attach(grid, label, 0, line, 2, 1);

  d->timezone = gtk_entry_new();
  gtk_widget_set_tooltip_text(d->timezone, _("start typing to show a list of permitted values and select your timezone.\npress enter to confirm, so that the asterisk * disappears"));
  d->timezone_changed = dt_ui_label_new("");
  gtk_entry_set_width_chars(GTK_ENTRY(d->timezone), 0);

  GtkWidget *timezone_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(timezone_box), d->timezone, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(timezone_box), d->timezone_changed, FALSE, FALSE, 0);

  gtk_grid_attach(grid, timezone_box, 2, line++, 2, 1);

  GtkCellRenderer *renderer;
  GtkTreeIter tree_iter;
  GtkListStore *model = gtk_list_store_new(2, G_TYPE_STRING /*display*/, G_TYPE_STRING /*name*/);
  GtkWidget *tz_selection = gtk_combo_box_new_with_model(GTK_TREE_MODEL(model));
  renderer = gtk_cell_renderer_text_new();
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(tz_selection), renderer, FALSE);
  gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(tz_selection), renderer, "text", 0, NULL);

  gchar *tz = dt_conf_get_string("plugins/lighttable/geotagging/tz");
  d->tz_camera = (tz == NULL) ? g_time_zone_new_utc() : g_time_zone_new(tz);
  for(GList *iter = d->timezones; iter; iter = g_list_next(iter))
  {
    tz_tuple_t *tz_tuple = (tz_tuple_t *)iter->data;
    gtk_list_store_append(model, &tree_iter);
    gtk_list_store_set(model, &tree_iter, 0, tz_tuple->display, 1, tz_tuple->name, -1);
    if(!strcmp(tz_tuple->name, tz))
      gtk_entry_set_text(GTK_ENTRY(d->timezone), tz_tuple->display);
  }
  g_free(tz);

  // add entry completion
  GtkEntryCompletion *completion = gtk_entry_completion_new();
  gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(model));
  gtk_entry_completion_set_text_column(completion, 0);
  gtk_entry_completion_set_inline_completion(completion, TRUE);
  gtk_entry_completion_set_popup_set_width(completion, FALSE);
  gtk_entry_completion_set_match_func(completion, _completion_match_func, NULL, NULL);
  gtk_entry_completion_set_minimum_key_length(completion, 0);
  gtk_entry_set_completion(GTK_ENTRY(d->timezone), completion);
  g_signal_connect(G_OBJECT(d->timezone), "key-press-event", G_CALLBACK(_timezone_key_pressed), self);
  g_signal_connect(G_OBJECT(d->timezone), "focus-out-event", G_CALLBACK(_timezone_focus_out), self);

  // gpx
  d->gpx_button = dt_action_button_new(self, N_("apply GPX track file..."), _choose_gpx_callback, self,
                                       _("parses a GPX file and updates location of selected images"), 0, 0);
  gtk_grid_attach(grid, d->gpx_button, 0, line++, 4, 1);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(grid), TRUE, TRUE, 0);
#ifdef HAVE_MAP
  grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  line = 0;

  d->map.gpx_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->map.gpx_section), TRUE, TRUE, 0);

  label = dt_ui_section_label_new(C_("section", "GPX file"));
  gtk_grid_attach(grid, label, 0, line++, 4, 1);

  d->map.gpx_button = dtgtk_button_new(dtgtk_cairo_paint_directory, CPF_NONE, NULL);
  gtk_widget_set_hexpand(d->map.gpx_button, FALSE);
  gtk_widget_set_halign(d->map.gpx_button, GTK_ALIGN_START);
  gtk_widget_set_name(d->map.gpx_button, "non-flat");
  gtk_widget_set_tooltip_text(d->map.gpx_button, _("select a GPX track file..."));
  gtk_grid_attach(grid, d->map.gpx_button, 0, line, 1, 1);
  g_signal_connect(G_OBJECT(d->map.gpx_button), "clicked", G_CALLBACK(_choose_gpx_callback), self);

  d->map.gpx_file = dt_ui_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(d->map.gpx_file ), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_hexpand(d->map.gpx_file, TRUE);
  gtk_grid_attach(grid, d->map.gpx_file, 1, line++, 3, 1);
  gtk_box_pack_start(GTK_BOX(d->map.gpx_section), GTK_WIDGET(grid), TRUE, TRUE, 0);

  model = gtk_list_store_new(DT_GEO_TRACKS_NUM_COLS, G_TYPE_BOOLEAN, G_TYPE_STRING,
                             G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING);

  d->map.gpx_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(model));
  g_object_unref(model);
  gtk_widget_set_name(d->map.gpx_view, "gpx_list");
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->map.gpx_view),
                              _("list of track segments in the GPX file, for each segment:"
                                "\n- the start date/time in local time (LT)"
                                "\n- the number of track points"
                                "\n- the number of matching images"
                                " based on images date/time, offset and time zone"
                                "\n- more detailed time information hovering the row"));
  renderer = gtk_cell_renderer_toggle_new();
  g_signal_connect(renderer, "toggled", G_CALLBACK(_track_seg_toggled), self);
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes("", renderer, "active", DT_GEO_TRACKS_ACTIVE, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->map.gpx_view), column);
  d->map.sel_tracks = column;
  GtkWidget *button = gtk_check_button_new();
  gtk_widget_show(button);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), FALSE);
  gtk_tree_view_column_set_widget(column, button);
  gtk_tree_view_column_set_alignment(column, 0.5);
  g_signal_connect(column, "clicked", G_CALLBACK(_all_tracks_toggled), self);

  column = _new_tree_text_column(_("start time"), TRUE, 0.0, DT_GEO_TRACKS_DATETIME, PANGO_ELLIPSIZE_START);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->map.gpx_view), column);
  column = _new_tree_text_column(_("points"), FALSE, 1.0, DT_GEO_TRACKS_POINTS, PANGO_ELLIPSIZE_NONE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->map.gpx_view), column);
  column = _new_tree_text_column(_("images"), FALSE, 1.0, DT_GEO_TRACKS_IMAGES, PANGO_ELLIPSIZE_NONE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->map.gpx_view), column);

  g_object_set(G_OBJECT(d->map.gpx_view), "has-tooltip", TRUE, NULL);
  g_signal_connect(G_OBJECT(d->map.gpx_view), "query-tooltip", G_CALLBACK(_row_tooltip_setup), self);

  // avoid ugly console pixman messages due to headers
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(d->map.gpx_view), FALSE);
  GtkWidget *w = dt_ui_resize_wrap(GTK_WIDGET(d->map.gpx_view), 100, "plugins/lighttable/geotagging/heighttracklist");
  gtk_widget_set_size_request(w, -1, 100);
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(d->map.gpx_view), TRUE);
  gtk_box_pack_start(GTK_BOX(d->map.gpx_section), w, TRUE, TRUE, 0);

  grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  line = 0;

  d->map.preview_button = gtk_check_button_new_with_label(_("preview images"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->map.preview_button), TRUE);
  gtk_widget_set_sensitive(d->map.preview_button, FALSE);
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->map.preview_button))), PANGO_ELLIPSIZE_END);
  gtk_grid_attach(grid, d->map.preview_button, 0, line, 1, 1);
  gtk_widget_set_tooltip_text(d->map.preview_button, _("show on map matching images"));
  g_signal_connect(GTK_TOGGLE_BUTTON(d->map.preview_button), "toggled", G_CALLBACK(_images_preview_toggled), self);

  d->map.select_button = dt_action_button_new(self, N_("select images"), _select_images, self,
                                              _("select matching images"), 0, 0);
  gtk_widget_set_hexpand(d->map.select_button, TRUE);
  gtk_widget_set_sensitive(d->map.select_button, FALSE);
  gtk_grid_attach(grid, d->map.select_button, 1, line, 1, 1);

  d->map.nb_imgs_label = dt_ui_label_new("0/0");
  gtk_widget_set_halign(d->map.nb_imgs_label, GTK_ALIGN_END);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->map.nb_imgs_label),
                              _("number of matching images versus selected images"));
  gtk_grid_attach(grid, d->map.nb_imgs_label, 2, line++, 1, 1);

  d->map.apply_gpx_button = dt_action_button_new(self, N_("apply geo-location"), _apply_gpx, self,
                                                 _("apply geo-location to matching images"), 0, 0);
  gtk_widget_set_hexpand(d->map.apply_gpx_button, TRUE);
  gtk_widget_set_sensitive(d->map.apply_gpx_button, FALSE);
  gtk_grid_attach(grid, d->map.apply_gpx_button, 0, line++, 3, 1);

  gtk_box_pack_start(GTK_BOX(d->map.gpx_section), GTK_WIDGET(grid), TRUE, TRUE, 0);

  d->map.view = FALSE;
  gtk_widget_show_all(self->widget);
  gtk_widget_set_no_show_all(self->widget, TRUE);
  _update_layout(self);
#endif
  d->imgid = 0;
  d->datetime = d->datetime0 = _get_image_datetime(self);
  if(d->datetime)
    g_date_time_ref(d->datetime);
  _display_datetime(&d->dt0, d->datetime0, FALSE, self);
  _display_datetime(&d->dt, d->datetime, TRUE, self);
  d->offset = 0;
  _display_offset(d->offset, TRUE, self);

  for(int i = 0; i < DT_GEOTAG_PARTS_NB; i++)
  {
    g_signal_connect(d->dt.widget[i], "changed", G_CALLBACK(_datetime_entry_changed), self);
    g_signal_connect(d->dt.widget[i], "key-press-event", G_CALLBACK(_datetime_key_pressed), self);
    g_signal_connect(d->dt.widget[i], "scroll-event", G_CALLBACK(_datetime_scroll_over), self);
  }
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED,
                            G_CALLBACK(_image_info_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE,
                            G_CALLBACK(_dt_pref_change_callback), self);
#ifdef HAVE_MAP
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,
                            G_CALLBACK(_view_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_GEOTAG_CHANGED,
                            G_CALLBACK(_geotag_changed), self);
#endif

  _show_milliseconds(d);
  gtk_widget_show_all(self->widget);
  gtk_widget_set_no_show_all(self->widget, TRUE);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  g_list_free_full(d->timezones, free_tz_tuple);
  d->timezones = NULL;
  g_time_zone_unref(d->tz_camera);
  if(d->datetime)
    g_date_time_unref(d->datetime);
  if(d->datetime0)
    g_date_time_unref(d->datetime0);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_mouse_over_image_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_image_info_changed), self);
  if(d->imgs)
  {
#ifdef HAVE_MAP
    _remove_images_from_map(self);
#endif
    g_list_free_full(d->imgs, g_free);
  }
  d->imgs = NULL;
  d->imgs = 0;
#ifdef HAVE_MAP
  _remove_tracks_from_map(self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_view_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_geotag_changed), self);
#endif
  free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
