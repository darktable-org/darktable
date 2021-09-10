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
#include "common/gpx.h"
#include "common/geo.h"
#include "common/darktable.h"
#include <glib.h>
#include <inttypes.h>

/* GPX XML parser */
typedef enum _gpx_parser_element_t
{
  GPX_PARSER_ELEMENT_NONE = 0,
  GPX_PARSER_ELEMENT_TRKPT = 1 << 0,
  GPX_PARSER_ELEMENT_TIME = 1 << 1,
  GPX_PARSER_ELEMENT_ELE = 1 << 2,
  GPX_PARSER_ELEMENT_NAME = 1 << 3
} _gpx_parser_element_t;

typedef struct dt_gpx_t
{
  /* the list of track records parsed */
  GList *trkpts;
  GList *trksegs;

  /* currently parsed track point */
  dt_gpx_track_point_t *current_track_point;
  _gpx_parser_element_t current_parser_element;
  gboolean invalid_track_point;
  gboolean parsing_trk;
  uint32_t segid;
  char *seg_name;
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
  const dt_gpx_track_point_t *pa = (const dt_gpx_track_point_t *)a;
  const dt_gpx_track_point_t *pb = (const dt_gpx_track_point_t *)b;
  return g_date_time_compare(pa->time, pb->time);
}

static gint _sort_segment(gconstpointer a, gconstpointer b)
{
  const dt_gpx_track_segment_t *pa = (const dt_gpx_track_segment_t *)a;
  const dt_gpx_track_segment_t *pb = (const dt_gpx_track_segment_t *)b;
  return g_date_time_compare(pa->start_dt, pb->start_dt);
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

  gpx->trkpts = g_list_sort(gpx->trkpts, _sort_track);
  gpx->trksegs = g_list_sort(gpx->trksegs, _sort_segment);

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

void _track_seg_free(dt_gpx_track_segment_t *trkseg)
{
  g_free(trkseg->name);
  g_free(trkseg);
}

void _track_pts_free(dt_gpx_track_point_t *trkpt)
{
  g_date_time_unref(trkpt->time);
  g_free(trkpt);
}

void dt_gpx_destroy(struct dt_gpx_t *gpx)
{
  g_assert(gpx != NULL);

  if(gpx->trkpts) g_list_free_full(gpx->trkpts, (GDestroyNotify)_track_pts_free);
  if(gpx->trksegs) g_list_free_full(gpx->trksegs, (GDestroyNotify)_track_seg_free);

  g_free(gpx);
}

gboolean dt_gpx_get_location(struct dt_gpx_t *gpx, GDateTime *timestamp, dt_image_geoloc_t *geoloc)
{
  g_assert(gpx != NULL);

  /* verify that we got at least 2 trackpoints */
  if(g_list_shorter_than(gpx->trkpts,2)) return FALSE;

  for(GList *item = gpx->trkpts; item; item = g_list_next(item))
  {
    dt_gpx_track_point_t *tp = (dt_gpx_track_point_t *)item->data;

    /* if timestamp is out of time range return false but fill
       closest location value start or end point */
    const gint cmp = g_date_time_compare(timestamp, tp->time);
    if((!item->next && cmp >= 0) || (cmp <= 0))
    {
      geoloc->longitude = tp->longitude;
      geoloc->latitude = tp->latitude;
      geoloc->elevation = tp->elevation;
      return FALSE;
    }

    dt_gpx_track_point_t *tp_next = (dt_gpx_track_point_t *)item->next->data;
    /* check if timestamp is within current and next trackpoint */
    const gint cmp_n = g_date_time_compare(timestamp, tp_next->time);
    if((cmp >= 0) && (item->next && cmp_n <= 0))
    {
      GTimeSpan seg_diff = g_date_time_difference(tp_next->time, tp->time);
      GTimeSpan diff = g_date_time_difference(timestamp, tp->time);
      if (seg_diff == 0)
      {
        geoloc->longitude = tp->longitude;
        geoloc->latitude = tp->latitude;
        geoloc->elevation = tp->elevation;
      }
      else
      {
        /* get the point by interpolation according to timestamp */
        float interpolator = (float)diff / (float)seg_diff;
        geoloc->longitude = tp->longitude + (tp_next->longitude - tp->longitude) * interpolator;
        geoloc->latitude = tp->latitude + (tp_next->latitude - tp->latitude) * interpolator;
        geoloc->elevation = tp->elevation + (tp_next->elevation - tp->elevation) * interpolator;
      }
      return TRUE;
    }
  }

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
      gpx->current_track_point = g_malloc0(sizeof(dt_gpx_track_point_t));
      gpx->current_track_point->segid = gpx->segid;

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
  else if(strcmp(element_name, "name") == 0)
  {
    gpx->current_parser_element = GPX_PARSER_ELEMENT_NAME;
  }
  else if(strcmp(element_name, "trkseg") == 0)
  {
    dt_gpx_track_segment_t *ts = g_malloc0(sizeof(dt_gpx_track_segment_t));
    ts->name = gpx->seg_name;
    ts->id = gpx->segid;
    gpx->seg_name = NULL;
    gpx->trksegs = g_list_prepend(gpx->trksegs, ts);
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
        gpx->trkpts = g_list_prepend(gpx->trkpts, gpx->current_track_point);
      else
        g_free(gpx->current_track_point);

      gpx->current_track_point = NULL;
    }
    else if(strcmp(element_name, "trkseg") == 0)
    {
      gpx->segid++;
    }

    /* clear current parser element */
    gpx->current_parser_element = GPX_PARSER_ELEMENT_NONE;
  }
}

void _gpx_parser_text(GMarkupParseContext *context, const gchar *text, gsize text_len, gpointer user_data,
                      GError **error)
{
  dt_gpx_t *gpx = (dt_gpx_t *)user_data;

  if(gpx->current_parser_element == GPX_PARSER_ELEMENT_NAME)
  {
    if(gpx->seg_name) g_free(gpx->seg_name);
    gpx->seg_name =  g_strdup(text);
  }

  if(!gpx->current_track_point) return;

  if(gpx->current_parser_element == GPX_PARSER_ELEMENT_TIME)
  {
    gpx->current_track_point->time = g_date_time_new_from_iso8601(text, NULL);
    if(!gpx->current_track_point->time)
    {
      gpx->invalid_track_point = TRUE;
      fprintf(stderr, "broken gpx file, failed to pars is8601 time '%s' for trackpoint\n", text);
    }
    dt_gpx_track_segment_t *ts = (dt_gpx_track_segment_t *)gpx->trksegs->data;
    if(ts)
    {
      ts->nb_trkpt++;
      if(!ts->start_dt)
      {
        ts->start_dt = gpx->current_track_point->time;
        ts->trkpt = gpx->current_track_point;
      }
      ts->end_dt = gpx->current_track_point->time;
    }
  }
  else if(gpx->current_parser_element == GPX_PARSER_ELEMENT_ELE)
    gpx->current_track_point->elevation = g_ascii_strtod(text, NULL);
}

GList *dt_gpx_get_trkseg(struct dt_gpx_t *gpx)
{
  return gpx->trksegs;
}

GList *dt_gpx_get_trkpts(struct dt_gpx_t *gpx, const guint segid)
{
  GList *pts = NULL;
  GList *ts = g_list_nth(gpx->trksegs, segid);
  if(!ts) return pts;
  dt_gpx_track_segment_t *tsd = (dt_gpx_track_segment_t *)ts->data;
  GList *tps = g_list_find(gpx->trkpts, tsd->trkpt);
  if(!tps) return pts;
  for(GList *tp = tps; tp; tp = g_list_next(tp))
  {
    dt_gpx_track_point_t *tpd = (dt_gpx_track_point_t *)tp->data;
    if(tpd->segid != segid) return pts;
    dt_geo_map_display_point_t *p = g_malloc0(sizeof(dt_geo_map_display_point_t));
    p->lat = tpd->latitude;
    p->lon = tpd->longitude;
    pts = g_list_prepend(pts, p);
  }
  return pts;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
