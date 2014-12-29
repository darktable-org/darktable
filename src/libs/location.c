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
#include <curl/curl.h>
#include "common/darktable.h"
#include "control/jobs.h"
#include "control/control.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/gtk.h"
#include "dtgtk/icon.h"
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef struct dt_lib_location_t
{
  GtkEntry *search;
  GtkWidget *result;

  GList *places;

  /* result buffer written to by */
  gchar *response;
  size_t response_size;

} dt_lib_location_t;

typedef enum _lib_location_type_t
{
  LOCATION_TYPE_VILLAGE,
  LOCATION_TYPE_HAMLET,
  LOCATION_TYPE_CITY,
  LOCATION_TYPE_ADMINISTRATIVE,
  LOCATION_TYPE_RESIDENTAL,
  LOCATION_TYPE_UNKNOWN
} _lib_location_type_t;

typedef struct _lib_location_result_t
{
  int32_t relevance;
  _lib_location_type_t type;
  float lon;
  float lat;
  gchar *name;

} _lib_location_result_t;


#define LIMIT_RESULT 5

/* entry value committed, perform a search */
static void _lib_location_entry_activated(GtkButton *button, gpointer user_data);

static gboolean _lib_location_result_item_activated(GtkButton *button, GdkEventButton *ev, gpointer user_data);

static void _lib_location_parser_start_element(GMarkupParseContext *cxt, const char *element_name,
                                               const char **attribute_names, const gchar **attribute_values,
                                               gpointer user_data, GError **error);

const char *name()
{
  return _("find location");
}

uint32_t views()
{
  return DT_VIEW_MAP;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}


void gui_reset(dt_lib_module_t *self)
{
}

int position()
{
  return 999;
}

/*
  http://nominatim.openstreetmap.org/search/norrköping?format=xml&limit=5
 */
void gui_init(dt_lib_module_t *self)
{
  self->data = calloc(1, sizeof(dt_lib_location_t));
  dt_lib_location_t *lib = self->data;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

  /* add search box */
  lib->search = GTK_ENTRY(gtk_entry_new());
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(lib->search));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(lib->search), FALSE, FALSE, 0);

  g_signal_connect(G_OBJECT(lib->search), "activate", G_CALLBACK(_lib_location_entry_activated),
                   (gpointer)self);

  /* add result vbox */
  lib->result = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(lib->result), TRUE, FALSE, 2);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_location_t *lib = self->data;
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(lib->search));
  free(lib);
}

static GtkWidget *_lib_location_place_widget_new(_lib_location_result_t *place)
{
  GtkWidget *eb, *hb, *vb, *w;
  char location[512];
  eb = gtk_event_box_new();
  hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

  /* add name */
  w = gtk_label_new(place->name);
  gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
  gtk_widget_set_halign(w, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(vb), w, FALSE, FALSE, 0);

  /* add location coord */
  g_snprintf(location, sizeof(location), "lat: %.4f lon: %.4f", place->lat, place->lon);
  w = gtk_label_new(location);
  gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
  gtk_widget_set_halign(w, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(vb), w, FALSE, FALSE, 0);

  /* type icon */
  GtkWidget *icon = dtgtk_icon_new(dtgtk_cairo_paint_store, 0);

  /* setup layout */
  gtk_box_pack_start(GTK_BOX(hb), icon, FALSE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(hb), vb, FALSE, FALSE, 2);
  gtk_container_add(GTK_CONTAINER(eb), hb);

  gtk_widget_show_all(eb);

  /* connect button press signal for result item */
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_lib_location_result_item_activated),
                   (gpointer)place);


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
    case LOCATION_TYPE_RESIDENTAL:
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

/* called when search job has been processed and
   result has been parsed */
static void _lib_location_search_finish(gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_location_t *lib = (dt_lib_location_t *)self->data;

  /* check if search gave us some result */
  if(!lib->places) return;

  /* for each location found populate the result list */
  GList *item = lib->places;
  do
  {
    _lib_location_result_t *place = (_lib_location_result_t *)item->data;
    gtk_box_pack_start(GTK_BOX(lib->result), _lib_location_place_widget_new(place), TRUE, TRUE, 2);
    gtk_widget_show(lib->result);
  } while((item = g_list_next(item)) != NULL);

  /* if we only got one search result back lets
     set center location and zoom based on place type  */
  if(g_list_length(lib->places) == 1)
  {
    int32_t zoom = 0;
    _lib_location_result_t *item = (_lib_location_result_t *)lib->places->data;
    zoom = _lib_location_place_get_zoom(item);
    dt_view_map_center_on_location(darktable.view_manager, item->lon, item->lat, zoom);
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
  g_free(lib->response);
  lib->response = NULL;
  lib->response_size = 0;

  g_list_free_full(lib->places, g_free);
  lib->places = NULL;

  gtk_container_foreach(GTK_CONTAINER(lib->result), (GtkCallback)gtk_widget_destroy, NULL);

  /* build the query url */
  query = dt_util_dstrcat(query, "http://nominatim.openstreetmap.org/search/%s?format=xml&limit=%d", text,
                          LIMIT_RESULT);
  /* load url */
  curl = curl_easy_init();
  if(!curl) goto bail_out;

  curl_easy_setopt(curl, CURLOPT_URL, query);
  // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, lib);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _lib_location_curl_write_data);

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

  while(item)
  {
    _lib_location_result_t *p = (_lib_location_result_t *)item->data;
    fprintf(stderr, "(%f,%f) %s\n", p->lon, p->lat, p->name);
    item = g_list_next(item);
  }

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
  _lib_location_result_t *p = (_lib_location_result_t *)user_data;
  int32_t zoom = _lib_location_place_get_zoom(p);
  fprintf(stderr, "zoom to: %d\n", zoom);
  dt_view_map_center_on_location(darktable.view_manager, p->lon, p->lat, zoom);
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
                                               gpointer user_data, GError **error)
{
  dt_lib_location_t *lib = (dt_lib_location_t *)user_data;

  /* only interested in place element */
  if(strcmp(element_name, "place") != 0) return;

  /* create new place */
  _lib_location_result_t *place = g_malloc0(sizeof(_lib_location_result_t));
  if(!place) return;

  place->lon = NAN;
  place->lat = NAN;

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
      else if(strcmp(*aname, "type") == 0)
      {

        if(strcmp(*avalue, "village") == 0)
          place->type = LOCATION_TYPE_RESIDENTAL;
        else if(strcmp(*avalue, "hamlet") == 0)
          place->type = LOCATION_TYPE_HAMLET;
        else if(strcmp(*avalue, "city") == 0)
          place->type = LOCATION_TYPE_CITY;
        else if(strcmp(*avalue, "administrative") == 0)
          place->type = LOCATION_TYPE_ADMINISTRATIVE;
        else if(strcmp(*avalue, "residental") == 0)
          place->type = LOCATION_TYPE_RESIDENTAL;
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
