/*
    This file is part of darktable,
    copyright (c) 2017 Ronny Kahl.

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
#include "common/revgeocode.h"
#include "common/darktable.h"
#include "common/debug.h"
// #include "control/conf.h"
#include "control/jobs/control_jobs.h"
#include <glib.h>
#include <curl/curl.h>

#define FLOAT_PLACES 8
#define FLOAT_BUFLEN (FLOAT_PLACES+6)  // sign + int part + decimal point + EOS

typedef struct _rev_geocode_data_t
{
  double lat, lon;
  int    location_id;
  gboolean has_lat_lon, has_location_id;
  gboolean list_changed;
} _rev_geocode_data_t;

typedef struct _rev_geocode_curl_response_t
{
  gchar *buf;
  size_t size;
} _rev_geocode_curl_response_t;

static gint _rev_geocode_location_id_usage(gint location_id)
{
  gint cnt = 0;
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "SELECT COUNT(images.id) FROM main.images WHERE location_id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, location_id);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    cnt = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);

  return cnt;
}

static void _rev_geocode_get_data(gint imgid, _rev_geocode_data_t *data)
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "SELECT latitude,longitude,location_id FROM main.images WHERE id = ?1",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(sqlite3_column_type(stmt, 0) == SQLITE_NULL || sqlite3_column_type(stmt, 1) == SQLITE_NULL)
      data->has_lat_lon = FALSE;
    else
      data->has_lat_lon = TRUE;
    data->lat = sqlite3_column_double(stmt, 0);
    data->lon = sqlite3_column_double(stmt, 1);
    if(sqlite3_column_type(stmt, 2) == SQLITE_NULL)
      data->has_location_id = FALSE;
    else
      data->has_location_id = TRUE;
    data->location_id = sqlite3_column_int(stmt, 2);
  }
  sqlite3_finalize(stmt);
}

static dt_rev_geocode_status_t _rev_geocode_remove_location(gint imgid, _rev_geocode_data_t *data)
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "UPDATE main.images SET location_id=NULL WHERE id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if(data->location_id > 0)
    if(_rev_geocode_location_id_usage(data->location_id) == 0)
      data->list_changed = TRUE;
  return DT_REV_GEOCODE_STATUS_REMOVED;
}

static gint _rev_geocode_insert_location(gchar *location_name)
{
  gint rc;
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "INSERT INTO data.locations (id, name) VALUES (NULL, ?1)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, location_name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "SELECT id FROM data.locations WHERE name = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, location_name, -1, SQLITE_TRANSIENT);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    rc = sqlite3_column_int64(stmt, 0);
  else
    rc = -1;
  sqlite3_finalize(stmt);
  return rc;
}

static gint _rev_geocode_update_location(gchar *location_name)
{
  gint rc = -1;
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "SELECT id FROM data.locations WHERE name = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, location_name, -1, SQLITE_TRANSIENT);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    rc = sqlite3_column_int64(stmt, 0);
  else
    rc = _rev_geocode_insert_location(location_name);
  sqlite3_finalize(stmt);
  return rc;
}

static dt_rev_geocode_status_t _rev_geocode_set_location_id(gint imgid, gint location_id, _rev_geocode_data_t *data)
{
  sqlite3_stmt *stmt;

  if(!data->has_location_id || location_id != data->location_id)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
        "UPDATE main.images SET location_id=?2 WHERE id = ?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, location_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    data->list_changed = TRUE;
  }
  return DT_REV_GEOCODE_STATUS_SUCCESS;
}

static char *_rev_geocode_get_lang()
{
  static char lang[3]="";

  if(lang[0]=='\0')
  {
    const char *env=g_getenv("LANG");
    if(strlen(env)>1)
      strncpy(lang, env, 2);
    else
      strncpy(lang, "en", 2);
  }
  return lang;
}

static char *_rev_geocode_sprintfloat(double in, char *buf)
{
  unsigned int i, d;
  char *fmt = "%s%u.%0*u";
  double abs_in = fabs(in);

  abs_in=fabs(in);
  i = (int)abs_in;
  d = (int)((abs_in-i)*pow(10, FLOAT_PLACES));
  snprintf(buf, FLOAT_BUFLEN, fmt, (in<0.0)?"-":"", i, FLOAT_PLACES, d);
  return buf;
}

static size_t _rev_geocode_curl_writer(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;
  struct _rev_geocode_curl_response_t *mem = (_rev_geocode_curl_response_t *)userp;

  mem->buf = realloc(mem->buf, mem->size + realsize + 1);
  if(mem->buf == NULL)
    return 0;

  memcpy(&(mem->buf[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->buf[mem->size] = 0;

  return realsize;
}

static dt_rev_geocode_status_t _rev_geocode_query_osm(gchar *query, _rev_geocode_curl_response_t *response)
{
  dt_rev_geocode_status_t rc = DT_REV_GEOCODE_STATUS_CONNECT_ERROR;
  CURL *curl = NULL;
  CURLcode res;
  long response_code;

  if((curl = curl_easy_init()) != NULL)
  {
    curl_easy_setopt(curl, CURLOPT_URL, query);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, (char *)darktable_package_string);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _rev_geocode_curl_writer);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)response);
    res = curl_easy_perform(curl);
    switch(res)
    {
      case CURLE_OK:
        rc = DT_REV_GEOCODE_STATUS_SUCCESS;
        break;
      case CURLE_OPERATION_TIMEDOUT:
        fprintf(stderr, "HTTP timeout (%s)\n", query);
        rc = DT_REV_GEOCODE_STATUS_CONNECT_ERROR;
        break;
      case CURLE_HTTP_RETURNED_ERROR:
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        fprintf(stderr, "HTTP status code: %ld (%s)\n", response_code, query);
        rc = DT_REV_GEOCODE_STATUS_FAIL;
        break;
      case CURLE_COULDNT_CONNECT:
        fprintf(stderr, "HTTP connection failed (%s)\n", query);
        rc = DT_REV_GEOCODE_STATUS_CONNECT_ERROR;
        break;
      case CURLE_COULDNT_RESOLVE_HOST:
        fprintf(stderr, "Unable to resolve hostname (%s)\n", query);
        rc = DT_REV_GEOCODE_STATUS_CONNECT_ERROR;
        break;
      default:
        fprintf(stderr, "HTTP connection unexpected error %d (%s)\n", res, query);
        rc = DT_REV_GEOCODE_STATUS_CONNECT_ERROR;
        break;
    }
    curl_easy_cleanup(curl);
  }
  return rc;
}

static gchar *_rev_geocode_build_name(const gchar *country, const gchar *state, const gchar *district, const gchar *county)
{
  gchar *l = NULL;

  if(country)
  {
    l = g_strdup(country);
    if(state)
      l = dt_util_dstrcat(l, "|%s", state);
    if(district)
      l = dt_util_dstrcat(l, "|%s", district);
    if(county)
      l = dt_util_dstrcat(l, "|%s", county);
  }
  return l;
}

static const gchar *_rev_geocode_parse_response_member(JsonReader *reader, gchar *member)
{
  const gchar *value = NULL;

  if(json_reader_read_member(reader, member))
    value = json_reader_get_string_value(reader);
  json_reader_end_element(reader);

  return value;
}

static dt_rev_geocode_status_t _rev_geocode_parse_response(gint imgid, _rev_geocode_data_t *data, _rev_geocode_curl_response_t *response)
{
  dt_rev_geocode_status_t rc = DT_REV_GEOCODE_STATUS_FAIL;
  JsonParser *parser = json_parser_new();
  JsonNode *root;
  JsonReader *reader;
  const gchar *country, *state, *district, *county;
  gchar *location_name = NULL;

  if(json_parser_load_from_data(parser, response->buf, response->size, NULL))
  {
    root = json_parser_get_root(parser);
    reader = json_reader_new(root);
    if(json_reader_read_member(reader, "address"))
    {
      country = _rev_geocode_parse_response_member(reader, "country");
      state = _rev_geocode_parse_response_member(reader, "state");
      district = _rev_geocode_parse_response_member(reader, "state_district");
      county = _rev_geocode_parse_response_member(reader, "county");

      location_name = _rev_geocode_build_name(country, state, district, county);
      rc = _rev_geocode_set_location_id(imgid, _rev_geocode_update_location(location_name), data);

      g_free(location_name);
      g_object_unref(reader);
    }
    else   // usually "Unable to geocode"
      rc = _rev_geocode_set_location_id(imgid, -1, data);
  }
  else
    fprintf(stderr, "Error parsing:\n%s\n", response->buf);
  g_object_unref(parser);
  return rc;
}

static dt_rev_geocode_status_t _rev_geocode_lookup_location(gint imgid, _rev_geocode_data_t *data)
{
  dt_rev_geocode_status_t rc;
  static const gchar *url = "http://nominatim.openstreetmap.org/reverse";
  char s_lat[FLOAT_BUFLEN], s_lon[FLOAT_BUFLEN];
  gchar *query = NULL;
  _rev_geocode_curl_response_t response;

  response.buf = malloc(1);
  response.size = 0;
  response.buf[0] = '\0';

  _rev_geocode_sprintfloat(data->lat, s_lat);
  _rev_geocode_sprintfloat(data->lon, s_lon);
  query = dt_util_dstrcat(query, "%s?format=json&adressdetails=1&accept-language=%s&lat=%s&lon=%s", 
      url, _rev_geocode_get_lang(), s_lat, s_lon);

  // lookup location in OSM and parse response
  if((rc = _rev_geocode_query_osm(query, &response)) == DT_REV_GEOCODE_STATUS_SUCCESS)
    rc = _rev_geocode_parse_response(imgid, data, &response);

  // remove location_id if lookup was not successful
  if(rc != DT_REV_GEOCODE_STATUS_SUCCESS)
    _rev_geocode_remove_location(imgid, data);

  g_free(response.buf);
  g_free(query);
  return rc;
}

dt_rev_geocode_status_t dt_rev_geocode(gint imgid, gboolean perform_lookup)
{
  dt_rev_geocode_status_t rc;
  _rev_geocode_data_t data;

  data.has_lat_lon = data.has_location_id = FALSE;
  data.lat = data.lon = 0.0;
  data.list_changed = FALSE;
  _rev_geocode_get_data(imgid, &data);
  if(data.has_lat_lon)
    if(perform_lookup)
      rc = _rev_geocode_lookup_location(imgid, &data);
    else
      rc = DT_REV_GEOCODE_STATUS_NOTHINGTODO;
  else
    if(data.has_location_id)
      rc = _rev_geocode_remove_location(imgid, &data);
    else
      rc = DT_REV_GEOCODE_STATUS_NOTHINGTODO;

  if(rc == DT_REV_GEOCODE_STATUS_SUCCESS || rc == DT_REV_GEOCODE_STATUS_REMOVED)
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_LOCATION_CHANGED, data.list_changed);
  return rc;
}

void dt_rev_geocode_startup()
{
  GList *list = NULL;
  sqlite3_stmt *stmt = NULL;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "SELECT DISTINCT id FROM main.images WHERE "
      "((latitude IS NOT NULL AND longitude IS NOT NULL) AND location_id IS NULL) OR "
      "((latitude IS NULL OR longitude IS NULL) AND location_id IS NOT NULL)",
      -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    list = g_list_append(list, GINT_TO_POINTER(imgid));
  }
  sqlite3_finalize(stmt);

  dt_control_rev_geocode(list);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
