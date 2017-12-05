/*
    This file is part of darktable,
    copyright (c) 2011 henrik andersson

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
#include "common/gpx.h"
#include "common/darktable.h"
#include <glib.h>
#include <inttypes.h>

typedef struct _gpx_track_point_t
{
  gdouble longitude, latitude, elevation;
  GTimeVal time;
} _gpx_track_point_t;

/* GPX XML parser */
typedef enum _gpx_parser_element_t
{
  GPX_PARSER_ELEMENT_NONE = 0,
  GPX_PARSER_ELEMENT_TRKPT = 1 << 0,
  GPX_PARSER_ELEMENT_TIME = 1 << 1,
  GPX_PARSER_ELEMENT_ELE = 1 << 2
} _gpx_parser_element_t;

typedef struct dt_gpx_t
{
  /* the list of track records parsed */
  GList *track;

  /* currently parsed track point */
  _gpx_track_point_t *current_track_point;
  _gpx_parser_element_t current_parser_element;
  gboolean invalid_track_point;
  gboolean parsing_trk;

} dt_gpx_t;

static void _gpx_parser_start_element(GMarkupParseContext *ctx, const gchar *element_name,
                                      const gchar **attribute_names, const gchar **attribute_values,
                                      gpointer ueer_data, GError **error);
static void _gpx_parser_end_element(GMarkupParseContext *context, const gchar *element_name,
                                    gpointer user_data, GError **error);
static void _gpx_parser_text(GMarkupParseContext *context, const gchar *text, gsize text_len,
                             gpointer user_data, GError **error);

static GMarkupParser _gpx_parser
    = { _gpx_parser_start_element, _gpx_parser_end_element, _gpx_parser_text, NULL, NULL };


static gint _sort_track(gconstpointer a, gconstpointer b)
{
  const _gpx_track_point_t *pa = (const _gpx_track_point_t *)a;
  const _gpx_track_point_t *pb = (const _gpx_track_point_t *)b;
  glong diff = pa->time.tv_sec - pb->time.tv_sec;
  return diff != 0 ? diff : pa->time.tv_usec - pb->time.tv_usec;
}

dt_gpx_t *dt_gpx_new(const gchar *filename)
{
  dt_gpx_t *gpx = NULL;
  GMarkupParseContext *ctx = NULL;
  GError *err = NULL;
  GMappedFile *gpxmf = NULL;
  gchar *gpxmf_content = NULL;
  gint gpxmf_size = 0;
  gint bom_offset = 0;


  /* map gpx file to parse into memory */
  gpxmf = g_mapped_file_new(filename, FALSE, &err);
  if(err) goto error;

  gpxmf_content = g_mapped_file_get_contents(gpxmf);
  gpxmf_size = g_mapped_file_get_length(gpxmf);
  if(!gpxmf_content || gpxmf_size < 10) goto error;

  /* allocate new dt_gpx_t context */
  gpx = g_malloc0(sizeof(dt_gpx_t));

  /* skip UTF-8 BOM */
  if(gpxmf_size > 3 && gpxmf_content[0] == '\xef' && gpxmf_content[1] == '\xbb' && gpxmf_content[2] == '\xbf')
    bom_offset = 3;

  /* initialize the parser and start parse gpx xml data */
  ctx = g_markup_parse_context_new(&_gpx_parser, 0, gpx, NULL);
  g_markup_parse_context_parse(ctx, gpxmf_content + bom_offset, gpxmf_size - bom_offset, &err);
  if(err) goto error;


  /* cleanup and return gpx context */
  g_markup_parse_context_free(ctx);
  g_mapped_file_unref(gpxmf);

  /* safeguard against corrupt gpx files that have the points not ordered by time */
  gpx->track = g_list_sort(gpx->track, _sort_track);

  return gpx;

error:
  if(err)
  {
    fprintf(stderr, "dt_gpx_new: %s\n", err->message);
    g_error_free(err);
  }

  if(ctx) g_markup_parse_context_free(ctx);

  g_free(gpx);

  if(gpxmf) g_mapped_file_unref(gpxmf);

  return NULL;
}

void dt_gpx_destroy(struct dt_gpx_t *gpx)
{
  g_assert(gpx != NULL);

  if(gpx->track) g_list_free_full(gpx->track, g_free);

  g_free(gpx);
}

gboolean dt_gpx_get_location(struct dt_gpx_t *gpx, GTimeVal *timestamp, gdouble *lon, gdouble *lat,
                             gdouble *ele)
{
  g_assert(gpx != NULL);

  GList *item = g_list_first(gpx->track);

  /* verify that we got at least 2 trackpoints */
  if(!item || !item->next) return FALSE;

  do
  {

    _gpx_track_point_t *tp = (_gpx_track_point_t *)item->data;

    /* if timestamp is out of time range return false but fill
       closest location value start or end point */
    if((!item->next && timestamp->tv_sec >= tp->time.tv_sec) || (timestamp->tv_sec <= tp->time.tv_sec))
    {
      *lon = tp->longitude;
      *lat = tp->latitude;
      *ele = tp->elevation;
      return FALSE;
    }

    /* check if timestamp is within current and next trackpoint */
    if(timestamp->tv_sec >= tp->time.tv_sec
       && timestamp->tv_sec <= ((_gpx_track_point_t *)item->next->data)->time.tv_sec)
    {
      *lon = tp->longitude;
      *lat = tp->latitude;
      *ele = tp->elevation;
      return TRUE;
    }

  } while((item = g_list_next(item)) != NULL);

  /* should not reach this point */
  return FALSE;
}

/*
 * GPX XML parser code
 */
void _gpx_parser_start_element(GMarkupParseContext *ctx, const gchar *element_name,
                               const gchar **attribute_names, const gchar **attribute_values,
                               gpointer user_data, GError **error)
{
  dt_gpx_t *gpx = (dt_gpx_t *)user_data;

  if(gpx->parsing_trk == FALSE)
  {
    // we only parse tracks and its points, nothing else
    if(strcmp(element_name, "trk") == 0)
    {
      gpx->parsing_trk = TRUE;
    }
    goto end;
  }

  /* from here on, parse wpType data from track points */
  if(strcmp(element_name, "trkpt") == 0)
  {
    if(gpx->current_track_point)
    {
      fprintf(stderr, "broken gpx file, new trkpt element before the previous ended.\n");
      g_free(gpx->current_track_point);
    }

    const gchar **attribute_name = attribute_names;
    const gchar **attribute_value = attribute_values;

    gpx->invalid_track_point = FALSE;

    if(*attribute_name)
    {
      gpx->current_track_point = g_malloc0(sizeof(_gpx_track_point_t));

      /* initialize with NAN for validation check */
      gpx->current_track_point->longitude = NAN;
      gpx->current_track_point->latitude = NAN;
      gpx->current_track_point->elevation = NAN;

      /* go thru the attributes to find and get values of lon / lat*/
      while(*attribute_name)
      {
        if(strcmp(*attribute_name, "lon") == 0)
          gpx->current_track_point->longitude = g_ascii_strtod(*attribute_value, NULL);
        else if(strcmp(*attribute_name, "lat") == 0)
          gpx->current_track_point->latitude = g_ascii_strtod(*attribute_value, NULL);

        attribute_name++;
        attribute_value++;
      }

      /* validate that we actually got lon / lat attribute values */
      if(isnan(gpx->current_track_point->longitude) || isnan(gpx->current_track_point->latitude))
      {
        fprintf(stderr, "broken gpx file, failed to get lon/lat attribute values for trkpt\n");
        gpx->invalid_track_point = TRUE;
      }
    }
    else
      fprintf(stderr, "broken gpx file, trkpt element doesn't have lon/lat attributes\n");

    gpx->current_parser_element = GPX_PARSER_ELEMENT_TRKPT;
  }
  else if(strcmp(element_name, "time") == 0)
  {
    if(!gpx->current_track_point) goto element_error;

    gpx->current_parser_element = GPX_PARSER_ELEMENT_TIME;
  }
  else if(strcmp(element_name, "ele") == 0)
  {
    if(!gpx->current_track_point) goto element_error;

    gpx->current_parser_element = GPX_PARSER_ELEMENT_ELE;
  }

end:

  return;

element_error:
  fprintf(stderr, "broken gpx file, element '%s' found outside of trkpt.\n", element_name);
}

void _gpx_parser_end_element(GMarkupParseContext *context, const gchar *element_name, gpointer user_data,
                             GError **error)
{
  dt_gpx_t *gpx = (dt_gpx_t *)user_data;

  /* closing trackpoint lets take care of data parsed */
  if(gpx->parsing_trk == TRUE)
  {
    if(strcmp(element_name, "trk") == 0)
    {
      gpx->parsing_trk = FALSE;
    }
    else if(strcmp(element_name, "trkpt") == 0)
    {
      if(!gpx->invalid_track_point)
        gpx->track = g_list_append(gpx->track, gpx->current_track_point);
      else
        g_free(gpx->current_track_point);

      gpx->current_track_point = NULL;
    }

    /* clear current parser element */
    gpx->current_parser_element = GPX_PARSER_ELEMENT_NONE;
  }
}

void _gpx_parser_text(GMarkupParseContext *context, const gchar *text, gsize text_len, gpointer user_data,
                      GError **error)
{
  dt_gpx_t *gpx = (dt_gpx_t *)user_data;

  if(!gpx->current_track_point) return;

  if(gpx->current_parser_element == GPX_PARSER_ELEMENT_TIME)
  {

    if(!g_time_val_from_iso8601(text, &gpx->current_track_point->time))
    {
      gpx->invalid_track_point = TRUE;
      fprintf(stderr, "broken gpx file, failed to pars is8601 time '%s' for trackpoint\n", text);
    }
  }
  else if(gpx->current_parser_element == GPX_PARSER_ELEMENT_ELE)
    gpx->current_track_point->elevation = g_ascii_strtod(text, NULL);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
