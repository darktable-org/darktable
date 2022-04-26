/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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

#include "common/darktable.h"
#include "common/geo.h"
#include "common/curl_tools.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "dtgtk/icon.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include <curl/curl.h>
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef enum _lib_location_type_t
{
  LOCATION_TYPE_VILLAGE,
  LOCATION_TYPE_HAMLET,
  LOCATION_TYPE_CITY,
  LOCATION_TYPE_ADMINISTRATIVE,
  LOCATION_TYPE_RESIDENTIAL,
  LOCATION_TYPE_UNKNOWN
} _lib_location_type_t;

typedef struct _lib_location_result_t
{
  int32_t relevance;
  _lib_location_type_t type;
  float lon;
  float lat;
  dt_map_box_t bbox;
  dt_geo_map_display_t marker_type;
  GList *marker_points;
  gchar *name;
} _lib_location_result_t;

typedef struct dt_lib_location_t
{
  GtkEntry *search;
  GtkWidget *result;
  GList *callback_params;

  GList *places;

  /* result buffer written to by */
  gchar *response;
  size_t response_size;

  /* pin, track or polygon currently shown on the map */
  GObject *marker;
  dt_geo_map_display_t marker_type;

  /* remember the currently selected search result so we can put it into a preset */
  _lib_location_result_t *selected_location;

  // place used to keep biggest polygon
  GList *marker_points;
} dt_lib_location_t;

typedef struct _callback_param_t
{
  dt_lib_location_t *lib;
  _lib_location_result_t *result;
} _callback_param_t;

#define LIMIT_RESULT 5

/* entry value committed, perform a search */
static void _lib_location_entry_activated(GtkButton *button, gpointer user_data);

static gboolean _lib_location_result_item_activated(GtkButton *button, GdkEventButton *ev, gpointer user_data);

static void _lib_location_parser_start_element(GMarkupParseContext *cxt, const char *element_name,
                                               const char **attribute_names, const gchar **attribute_values,
                                               gpointer user_data, GError **error);

static void clear_search(dt_lib_location_t *lib);

const char *name(dt_lib_module_t *self)
{
  return _("find location");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"map", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}


void gui_reset(dt_lib_module_t *self)
{
  dt_lib_location_t *lib = (dt_lib_location_t *)self->data;
  gtk_entry_set_text(lib->search, "");
  clear_search(lib);
}

int position()
{
  return 999;
}

/*
  https://nominatim.openstreetmap.org/search/norrköping?format=xml&limit=5
 */
void gui_init(dt_lib_module_t *self)
{
  self->data = calloc(1, sizeof(dt_lib_location_t));
  dt_lib_location_t *lib = self->data;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  /* add search box */
  lib->search = GTK_ENTRY(gtk_entry_new());
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(lib->search), FALSE, FALSE, 0);

  g_signal_connect(G_OBJECT(lib->search), "activate", G_CALLBACK(_lib_location_entry_activated),
                   (gpointer)self);

  /* add result vbox */
  lib->result = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(lib->result), TRUE, FALSE, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}

static gboolean _event_box_enter_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  if(event->type == GDK_ENTER_NOTIFY)
    gtk_widget_set_state_flags(widget, GTK_STATE_FLAG_PRELIGHT, FALSE);
  else
    gtk_widget_unset_state_flags(widget, GTK_STATE_FLAG_PRELIGHT);

  return FALSE;
}

static GtkWidget *_lib_location_place_widget_new(dt_lib_location_t *lib, _lib_location_result_t *place)
{
  GtkWidget *eb, *vb, *w;
  eb = gtk_event_box_new();
  gtk_widget_set_name(eb, "dt-map-location");
  g_signal_connect(G_OBJECT(eb), "enter-notify-event", G_CALLBACK(_event_box_enter_leave), NULL);
  g_signal_connect(G_OBJECT(eb), "leave-notify-event", G_CALLBACK(_event_box_enter_leave), NULL);

  vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  /* add name */
  w = gtk_label_new(place->name);
  gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
  gtk_widget_set_halign(w, GTK_ALIGN_START);
  g_object_set(G_OBJECT(w), "xalign", 0.0, (gchar *)0);
  gtk_box_pack_start(GTK_BOX(vb), w, FALSE, FALSE, 0);

  /* add location coord */
  gchar *lat = dt_util_latitude_str(place->lat);
  gchar *lon = dt_util_longitude_str(place->lon);
  gchar *location = g_strconcat(lat, ", ", lon, NULL);
  w = gtk_label_new(location);
  g_free(lat);
  g_free(lon);
  g_free(location);
  gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
  gtk_widget_set_halign(w, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(vb), w, FALSE, FALSE, 0);

  /* setup layout */
  gtk_container_add(GTK_CONTAINER(eb), vb);

  gtk_widget_show_all(eb);

  /* connect button press signal for result item */
  _callback_param_t *param = (_callback_param_t *)malloc(sizeof(_callback_param_t));
  lib->callback_params = g_list_append(lib->callback_params, param);
  param->lib = lib;
  param->result = place;
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_lib_location_result_item_activated),
                   (gpointer)param);


  return eb;
}

static size_t _lib_location_curl_write_data(void *buffer, size_t size, size_t nmemb, void *userp)
{
  dt_lib_location_t *lib = (dt_lib_location_t *)userp;

  char *newdata = g_malloc0(lib->response_size + nmemb + 1);
  if(lib->response != NULL) memcpy(newdata, lib->response, lib->response_size);
  memcpy(newdata + lib->response_size, buffer, nmemb);
  g_free(lib->response);
  lib->response = newdata;
  lib->response_size += nmemb;

  return nmemb;
}


static GMarkupParser _lib_location_parser = { _lib_location_parser_start_element, NULL, NULL, NULL, NULL };


static int32_t _lib_location_place_get_zoom(_lib_location_result_t *place)
{
  switch(place->type)
  {
    case LOCATION_TYPE_RESIDENTIAL:
      return 18;

    case LOCATION_TYPE_ADMINISTRATIVE:
      return 17;

    case LOCATION_TYPE_VILLAGE:
      return 12;

    case LOCATION_TYPE_HAMLET:
    case LOCATION_TYPE_CITY:
    case LOCATION_TYPE_UNKNOWN:
    default:
      return 8;
  }

  /* should never get here */
  return 0;
}

static void _clear_markers(dt_lib_location_t *lib)
{
  if(lib->marker_type == MAP_DISPLAY_NONE) return;
  dt_view_map_remove_marker(darktable.view_manager, lib->marker_type, lib->marker);
  g_object_unref(lib->marker);
  lib->marker = NULL;
  lib->marker_type = MAP_DISPLAY_NONE;
}

static void free_location(_lib_location_result_t *location)
{
  g_free(location->name);
  g_list_free_full(location->marker_points, free);
  free(location);
}

static void clear_search(dt_lib_location_t *lib)
{
  g_free(lib->response);
  lib->response = NULL;
  lib->response_size = 0;
  lib->selected_location = NULL;

  g_list_free_full(lib->places, (GDestroyNotify)free_location);
  lib->places = NULL;

  dt_gui_container_destroy_children(GTK_CONTAINER(lib->result));
  g_list_free_full(lib->callback_params, free);
  lib->callback_params = NULL;

  _clear_markers(lib);
}

static void _show_location(dt_lib_location_t *lib, _lib_location_result_t *p)
{
  if(isnan(p->bbox.lon1) || isnan(p->bbox.lat1) || isnan(p->bbox.lon2) || isnan(p->bbox.lat2))
  {
    int32_t zoom = _lib_location_place_get_zoom(p);
    dt_view_map_center_on_location(darktable.view_manager, p->lon, p->lat, zoom);
  }
  else
  {
    dt_view_map_center_on_bbox(darktable.view_manager, p->bbox.lon1, p->bbox.lat1, p->bbox.lon2, p->bbox.lat2);
  }

  _clear_markers(lib);

  lib->marker = dt_view_map_add_marker(darktable.view_manager, p->marker_type, p->marker_points);
  lib->marker_type = p->marker_type;
  lib->selected_location = p;

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_LOCATION_CHANGED,
                                p->marker_type == MAP_DISPLAY_POLYGON ? p->marker_points : NULL);
}

/* called when search job has been processed and
   result has been parsed */
static void _lib_location_search_finish(gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_location_t *lib = (dt_lib_location_t *)self->data;

  /* check if search gave us some result */
  if(!lib->places) return;

  /* for each location found populate the result list */
  for(const GList *item = lib->places; item; item = g_list_next(item))
  {
    _lib_location_result_t *place = (_lib_location_result_t *)item->data;
    gtk_box_pack_start(GTK_BOX(lib->result), _lib_location_place_widget_new(lib, place), TRUE, TRUE, 0);
    gtk_widget_show(lib->result);
  }

  /* if we only got one search result back lets
     set center location and zoom based on place type  */
  if(g_list_is_singleton(lib->places))
  {
    _lib_location_result_t *place = (_lib_location_result_t *)lib->places->data;
    _show_location(lib, place);
  }
}

static gboolean _lib_location_search(gpointer user_data)
{
  GMarkupParseContext *ctx = NULL;
  CURL *curl = NULL;
  CURLcode res;
  GError *err = NULL;

  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_location_t *lib = (dt_lib_location_t *)self->data;
  gchar *query = NULL, *text = NULL;

  /* get escaped search text */
  text = g_uri_escape_string(gtk_entry_get_text(lib->search), NULL, FALSE);

  if(!(text && *text)) goto bail_out;

  /* clean up previous results before adding new */
  clear_search(lib);

  /* build the query url */
  const char *search_url = dt_conf_get_string_const("plugins/map/geotagging_search_url");
  query = g_strdup_printf(search_url, text, LIMIT_RESULT);
  /* load url */
  curl = curl_easy_init();
  if(!curl) goto bail_out;

  dt_curl_init(curl, FALSE);

  curl_easy_setopt(curl, CURLOPT_URL, query);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, lib);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _lib_location_curl_write_data);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, (char *)darktable_package_string);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

  res = curl_easy_perform(curl);
  if(res != 0) goto bail_out;

  if(!lib->response) goto bail_out;

  /* parse xml response and populate the result list */
  ctx = g_markup_parse_context_new(&_lib_location_parser, 0, lib, NULL);
  g_markup_parse_context_parse(ctx, lib->response, lib->response_size, &err);
  if(err) goto bail_out;

  /* add the places into the result list */
  GList *item = lib->places;
  if(!item) goto bail_out;

//   while(item)
//   {
//     _lib_location_result_t *p = (_lib_location_result_t *)item->data;
//     fprintf(stderr, "(%f,%f) %s\n", p->lon, p->lat, p->name);
//     item = g_list_next(item);
//   }

/* cleanup an exit search job */
bail_out:
  if(err)
  {
    fprintf(stderr, "location search: %s\n", err->message);
    g_error_free(err);
  }

  if(curl) curl_easy_cleanup(curl);

  g_free(text);
  g_free(query);

  if(ctx) g_markup_parse_context_free(ctx);

  /* enable the widgets */
  gtk_widget_set_sensitive(GTK_WIDGET(lib->search), TRUE);
  // gtk_widget_set_sensitive(lib->result, FALSE);

  return FALSE;
}

gboolean _lib_location_result_item_activated(GtkButton *button, GdkEventButton *ev, gpointer user_data)
{
  _callback_param_t *param = (_callback_param_t *)user_data;
  dt_lib_location_t *lib = param->lib;
  _lib_location_result_t *result = param->result;
  _show_location(lib, result);
  return TRUE;
}

void _lib_location_entry_activated(GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_location_t *lib = (dt_lib_location_t *)self->data;
  const gchar *text = gtk_entry_get_text(lib->search);
  if(!text || text[0] == '\0') return;

  /* lock the widget while search job is performing */
  gtk_widget_set_sensitive(GTK_WIDGET(lib->search), FALSE);
  // gtk_widget_set_sensitive(lib->result, FALSE);

  /* start a bg job for fetching results of a search */
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, _lib_location_search, user_data, _lib_location_search_finish);
}


static void _lib_location_parser_start_element(GMarkupParseContext *cxt, const char *element_name,
                                               const char **attribute_names, const gchar **attribute_values,
                                               gpointer user_data, GError **e)
{
  dt_lib_location_t *lib = (dt_lib_location_t *)user_data;

  /* only interested in place element */
  if(strcmp(element_name, "place") != 0) return;

  // used to keep the biggest polygon
  lib->marker_points = NULL;

  /* create new place */
  _lib_location_result_t *place = g_malloc0(sizeof(_lib_location_result_t));
  if(!place) return;

  place->lon = NAN;
  place->lat = NAN;
  place->bbox.lon1 = NAN;
  place->bbox.lat1 = NAN;
  place->bbox.lon2 = NAN;
  place->bbox.lat2 = NAN;
  place->marker_type = MAP_DISPLAY_NONE;
  place->marker_points = NULL;

  gboolean show_outline = dt_conf_get_bool("plugins/map/show_outline");
  int max_outline_nodes = dt_conf_get_int("plugins/map/max_outline_nodes");

  /* handle the element attribute values */
  const gchar **aname = attribute_names;
  const gchar **avalue = attribute_values;
  if(*aname)
  {
    while(*aname)
    {
      if(strcmp(*aname, "display_name") == 0)
      {
        place->name = g_strdup(*avalue);
        if(!(place->name)) goto bail_out;
      }
      else if(strcmp(*aname, "lon") == 0)
        place->lon = g_strtod(*avalue, NULL);
      else if(strcmp(*aname, "lat") == 0)
        place->lat = g_strtod(*avalue, NULL);
      else if(strcmp(*aname, "boundingbox") == 0)
      {
        char *endptr;
        float lon1, lat1, lon2, lat2;

        lat1 = g_ascii_strtod(*avalue, &endptr);
        if(*endptr != ',') goto broken_bbox;
        endptr++;

        lat2 = g_ascii_strtod(endptr, &endptr);
        if(*endptr != ',') goto broken_bbox;
        endptr++;

        lon1 = g_ascii_strtod(endptr, &endptr);
        if(*endptr != ',') goto broken_bbox;
        endptr++;

        lon2 = g_ascii_strtod(endptr, &endptr);
        if(*endptr != '\0') goto broken_bbox;

        place->bbox.lon1 = lon1;
        place->bbox.lat1 = lat1;
        place->bbox.lon2 = lon2;
        place->bbox.lat2 = lat2;
broken_bbox:
        ;
      }
      // only use the first 'geotext' entry
      else if(show_outline &&
              strcmp(*aname, "geotext") == 0 &&
              place->marker_type == MAP_DISPLAY_NONE)
      {
        if(g_str_has_prefix(*avalue, "POINT"))
        {
          char *endptr;
          float lon = g_ascii_strtod(*avalue + strlen("POINT("), &endptr);
          float lat = g_ascii_strtod(endptr, &endptr);
          if(*endptr == ')')
          {
            place->marker_type = MAP_DISPLAY_POINT;
            dt_geo_map_display_point_t *p = malloc(sizeof(dt_geo_map_display_point_t));
            p->lon = lon;
            p->lat = lat;
            place->marker_points = g_list_append(place->marker_points, p);
          }
        }
        else if(g_str_has_prefix(*avalue, "LINESTRING")
#ifdef HAVE_OSMGPSMAP_110_OR_NEWER
                || g_str_has_prefix(*avalue, "POLYGON")
                || g_str_has_prefix(*avalue, "MULTIPOLYGON")
#endif
               )
        {
          gboolean error = FALSE;
          const char *startptr = *avalue;
          char *endptr;
          while(startptr && (*startptr == ' ' || *startptr == '(' || (*startptr >= 'A' && *startptr <= 'Z')))
            startptr++;

          int i = 0;
          while(1)
          {
            float lon = g_ascii_strtod(startptr, &endptr);
            float lat = g_ascii_strtod(endptr, &endptr);

            if(*endptr == ')') // TODO: support holes in POLYGON and several forms in MULTIPOLYGON?
            {
              // doesn't really support MULTIPOLYGON, just keeps the biggect one
              const int old_mp = g_list_length(lib->marker_points);
              const int new_mp = g_list_length(place->marker_points);
              if(g_str_has_prefix(endptr, ")),((") || g_str_has_prefix(endptr, "),("))
              {
                if(new_mp > old_mp)
                {
                  g_list_free_full(lib->marker_points, g_free);
                  lib->marker_points = place->marker_points;
                }
                else
                  g_list_free_full(place->marker_points, g_free);
                place->marker_points = NULL;
                startptr = endptr + (g_str_has_prefix(endptr, ")),((") ? 5 : 3);
                continue;
              }
              else
              {
                if(new_mp > old_mp)
                  g_list_free_full(lib->marker_points, g_free);
                else
                {
                  g_list_free_full(place->marker_points, g_free);
                  place->marker_points = lib->marker_points;
                }
                lib->marker_points = NULL;
                break;
              }
            }
            if(*endptr != ',' || i > max_outline_nodes) // don't go too big for speed reasons
            {
              error = TRUE;
              break;
            }
            dt_geo_map_display_point_t *p = malloc(sizeof(dt_geo_map_display_point_t));
            p->lon = lon;
            p->lat = lat;
            place->marker_points = g_list_prepend(place->marker_points, p);
            startptr = endptr+1;
            i++;
          }
          place->marker_points = g_list_reverse(place->marker_points);
          if(error)
          {
            g_list_free_full(place->marker_points, free);
            place->marker_points = NULL;
          }
          else
          {
            place->marker_type = g_str_has_prefix(*avalue, "LINESTRING") ? MAP_DISPLAY_TRACK : MAP_DISPLAY_POLYGON;
          }
        }
        else
        {
          gchar *s = g_strndup(*avalue, 100);
          fprintf(stderr, "unsupported outline: %s%s\n", s, strlen(s) == strlen(*avalue) ? "" : " ...");
          g_free(s);
        }
      }
      else if(strcmp(*aname, "type") == 0)
      {

        if(strcmp(*avalue, "village") == 0)
          place->type = LOCATION_TYPE_RESIDENTIAL;
        else if(strcmp(*avalue, "hamlet") == 0)
          place->type = LOCATION_TYPE_HAMLET;
        else if(strcmp(*avalue, "city") == 0)
          place->type = LOCATION_TYPE_CITY;
        else if(strcmp(*avalue, "administrative") == 0)
          place->type = LOCATION_TYPE_ADMINISTRATIVE;
        else if(strcmp(*avalue, "residental") == 0) // for backward compatibility
          place->type = LOCATION_TYPE_RESIDENTIAL;
        else if(strcmp(*avalue, "residential") == 0)
          place->type = LOCATION_TYPE_RESIDENTIAL;
      }

      aname++;
      avalue++;
    }
  }

  /* check if we got sane data */
  if(isnan(place->lon) || isnan(place->lat)) goto bail_out;

  /* add place to result list */
  lib->places = g_list_append(lib->places, place);

  return;

bail_out:
  g_free(place->name);
  g_free(place);
}

struct params_fixed_t
{
  int32_t relevance;
  _lib_location_type_t type;
  float lon;
  float lat;
  dt_map_box_t bbox;
  dt_geo_map_display_t marker_type;
} __attribute__((packed));

void *get_params(dt_lib_module_t *self, int *size)
{
  dt_lib_location_t *lib = (dt_lib_location_t *)self->data;
  _lib_location_result_t *location = lib->selected_location;

  // we have nothing to store when the user hasn't picked a search result
  if(location == NULL) return NULL;

  const size_t size_fixed = sizeof(struct params_fixed_t);
  const size_t size_name = strlen(location->name) + 1;
  const size_t size_points = 2 * sizeof(float) * g_list_length(location->marker_points);
  const size_t size_total = size_fixed + size_name + size_points;

  void *params = malloc(size_total);
  struct params_fixed_t *params_fixed = (struct params_fixed_t *)params;
  params_fixed->relevance = location->relevance;
  params_fixed->type = location->type;
  params_fixed->lon = location->lon;
  params_fixed->lat = location->lat;
  params_fixed->bbox.lon1 = location->bbox.lon1;
  params_fixed->bbox.lat1 = location->bbox.lat1;
  params_fixed->bbox.lon2 = location->bbox.lon2;
  params_fixed->bbox.lat2 = location->bbox.lat2;
  params_fixed->marker_type = location->marker_type;

  memcpy((uint8_t *)params + size_fixed, location->name, size_name);

  float *points = (float *)((uint8_t *)params + size_fixed + size_name);
  for(GList *iter = location->marker_points; iter; iter = g_list_next(iter), points += 2)
  {
    dt_geo_map_display_point_t *point = (dt_geo_map_display_point_t *)iter->data;
    points[0] = point->lat;
    points[1] = point->lon;
  }

  *size = size_total;
  return params;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  dt_lib_location_t *lib = (dt_lib_location_t *)self->data;

  const size_t size_fixed = sizeof(struct params_fixed_t);

  if(size < size_fixed) return 1;

  const struct params_fixed_t *params_fixed = (struct params_fixed_t *)params;
  const char *name = (char *)((uint8_t *)params + size_fixed);
  const size_t size_name = strlen(name) + 1;

  if(size_fixed + size_name > size) return 1;

  const size_t size_points = size - (size_fixed + size_name);

  if(size_points % 2 * sizeof(float) != 0) return 1;

  _lib_location_result_t *location = (_lib_location_result_t *)malloc(sizeof(_lib_location_result_t));

  location->relevance = params_fixed->relevance;
  location->type = params_fixed->type;
  location->lon = params_fixed->lon;
  location->lat = params_fixed->lat;
  location->bbox.lon1 = params_fixed->bbox.lon1;
  location->bbox.lat1 = params_fixed->bbox.lat1;
  location->bbox.lon2 = params_fixed->bbox.lon2;
  location->bbox.lat2 = params_fixed->bbox.lat2;
  location->marker_type = params_fixed->marker_type;
  location->name = g_strdup(name);
  location->marker_points = NULL;

  for(const float *points = (float *)((uint8_t *)params + size_fixed + size_name); (uint8_t *)points < (uint8_t *)params + size; points += 2)
  {
    dt_geo_map_display_point_t *p = (dt_geo_map_display_point_t *)malloc(sizeof(dt_geo_map_display_point_t));
    p->lat = points[0];
    p->lon = points[1];
    location->marker_points = g_list_prepend(location->marker_points, p);
  }
  location->marker_points = g_list_reverse(location->marker_points);

  clear_search(lib);
  lib->places = g_list_append(lib->places, location);
  gtk_entry_set_text(lib->search, "");
  _lib_location_search_finish(self);

  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

