/*
    This file is part of darktable,
    Copyright (C) 2022 darktable developers.

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
#include "common/datetime.h"

#define DT_DATETIME_ORIGIN "0001-01-01 00:00:00.000"
#define DT_DATETIME_EPOCH "1970-01-01 00:00:00.000"
#define DT_DATETIME_EXIF_FORMAT "%Y:%m:%d %H:%M:%S"

void dt_datetime_init()
{
  darktable.utc_tz =  g_time_zone_new_utc();
  darktable.origin_gdt = g_date_time_new_from_iso8601(DT_DATETIME_ORIGIN, darktable.utc_tz);
}

gboolean _datetime_gdatetime_to_numbers(dt_datetime_t *dt, GDateTime *gdt)
{
  if(gdt)
  {
    dt->year = g_date_time_get_year(gdt);
    dt->month = g_date_time_get_month(gdt);
    dt->day = g_date_time_get_day_of_month(gdt);
    dt->hour = g_date_time_get_hour(gdt);
    dt->minute = g_date_time_get_minute(gdt);
    dt->second = g_date_time_get_second(gdt);
    dt->msec = g_date_time_get_microsecond(gdt) * 0.001;
    return TRUE;
  }
  return FALSE;
}

static char *_datetime_append_msec(char *exif, GDateTime *gdt)
{
  return g_strdup_printf("%s%s%03d", exif, ".", (int)(g_date_time_get_microsecond(gdt) * 0.001));
  }

static GTimeSpan _gdatetime_to_gtimespan(GDateTime *gdt)
{
  if(gdt)
  {
    GTimeSpan gts = g_date_time_difference(gdt, darktable.origin_gdt);
    g_date_time_unref(gdt);
    return gts;
  }
  return 0;
}

gboolean dt_datetime_exif_to_numbers(dt_datetime_t *dt, const char *exif)
{
  if(exif && *exif && dt)
  {
    char sdt[DT_DATETIME_LENGTH] = DT_DATETIME_ORIGIN;
    int len = strlen(exif);
    // If TZ data is found in the datetime string we should discard it.
    // We will memcpy this string for parsing and we have to know where to stop so that
    // the TZ tail after shorter XMP date-time string doesn't damage our parsing buffer.
    // For possible formats see https://developer.adobe.com/xmp/docs/XMPNamespaces/XMPDataTypes/#date
    if(exif[len-1] == 'Z')
      len--;
    else if(exif[len-3] == '+' || exif[len-3] == '-')
      len -= 3;
    else if(exif[len-6] == '+' || exif[len-6] == '-')
      len -= 6;
    len = len > sizeof(sdt) - 1 ? sizeof(sdt) - 1 : len;
    memcpy(sdt, exif, len);
    sdt[4] = sdt[7] = '-';
    GDateTime *gdt = g_date_time_new_from_iso8601(sdt, darktable.utc_tz);
    if(gdt)
    {
      const gboolean res = _datetime_gdatetime_to_numbers(dt, gdt);
      g_date_time_unref(gdt);
      return res;
    }
  }
  return FALSE;
}

gboolean dt_datetime_exif_to_numbers_raw(dt_datetime_t *dt, const char *exif)
{
  if(exif && *exif && dt)
  {
    GMatchInfo *match_info;
    // we capture each date componenent
    GRegex *regex = g_regex_new(
        "^\\s*(\\d{4})?(?::(\\d{2}))?(?::(\\d{2}))?(?: (\\d{2}))?(?::(\\d{2}))?(?::(\\d{2}))?\\s*$", 0, 0, NULL);
    g_regex_match_full(regex, exif, -1, 0, 0, &match_info, NULL);
    int match_count = g_match_info_get_match_count(match_info);
    if(match_count == 7)
    {
      dt->year = atoi(g_match_info_fetch(match_info, 1));
      dt->month = atoi(g_match_info_fetch(match_info, 2));
      dt->day = atoi(g_match_info_fetch(match_info, 3));
      dt->hour = atoi(g_match_info_fetch(match_info, 4));
      dt->minute = atoi(g_match_info_fetch(match_info, 5));
      dt->second = atoi(g_match_info_fetch(match_info, 6));
      g_match_info_free(match_info);
      g_regex_unref(regex);
      return TRUE;
    }
    g_match_info_free(match_info);
    g_regex_unref(regex);
  }
  return FALSE;
}

gboolean dt_datetime_gdatetime_to_local(char *local, const size_t local_size,
                                        GDateTime *gdt, const gboolean msec, const gboolean tz)
{
  if(!local || !local_size || !gdt) return FALSE;
  local[0] = '\0';
  if(gdt)
  {
    char *sdt;
    if(tz)
    {
      GDateTime *lgdt = g_date_time_to_local(gdt);
      sdt = g_date_time_format(lgdt, "%a %x %X");
      g_date_time_unref(lgdt);
    }
    else
      sdt = g_date_time_format(gdt, "%a %x %X");
    if(sdt)
    {
      if(msec)
      { // add milliseconds
        char *sdt2 = _datetime_append_msec(sdt, gdt);
        g_free(sdt);
        sdt = sdt2;
      }
      g_strlcpy(local, sdt, local_size);
      g_free(sdt);
      return TRUE;
    }
  }
  return FALSE;
}

gboolean dt_datetime_gtimespan_to_local(char *local, const size_t local_size,
                                        const GTimeSpan gts, const gboolean msec, const gboolean tz)
{
  gboolean res = FALSE;
  if(!local || !local_size) return FALSE;
  local[0] = '\0';
  GDateTime *gdt = g_date_time_add(darktable.origin_gdt, gts);
  if(gdt)
  {
    res = dt_datetime_gdatetime_to_local(local, local_size, gdt, msec, tz);
    g_date_time_unref(gdt);
  }
  return res;
}

gboolean dt_datetime_img_to_local(char *local, const size_t local_size,
                                  const dt_image_t *img, const gboolean msec)
{
  return dt_datetime_gtimespan_to_local(local, local_size, img->exif_datetime_taken, msec, FALSE);
}

gboolean dt_datetime_unix_to_img(dt_image_t *img, const time_t *unix)
{
  GDateTime *gdt = g_date_time_new_from_unix_local(*unix);
  if(gdt)
  {
    img->exif_datetime_taken = g_date_time_difference(gdt, darktable.origin_gdt);
    g_date_time_unref(gdt);
    return TRUE;
  }
  img->exif_datetime_taken = 0;
  return FALSE;
}

gboolean dt_datetime_unix_to_exif(char *exif, const size_t exif_size, const time_t *unix)
{
  GDateTime *gdt = g_date_time_new_from_unix_local(*unix);
  if(gdt)
  {
    const gboolean res = dt_datetime_gdatetime_to_exif(exif, exif_size, gdt);
    g_date_time_unref(gdt);
    return res;
  }
  return FALSE;
}

void dt_datetime_now_to_exif(char *exif)
{
  if(!exif) return;
  exif[0] = '\0';
  GDateTime *gdt = g_date_time_new_now_local();
  if(gdt)
  {
    dt_datetime_gdatetime_to_exif(exif, DT_DATETIME_EXIF_LENGTH, gdt);
    g_date_time_unref(gdt);
  }
}

GTimeSpan dt_datetime_now_to_gtimespan()
{
  GDateTime *gdt = g_date_time_new_now_local();
  return _gdatetime_to_gtimespan(gdt);
}

void dt_datetime_exif_to_img(dt_image_t *img, const char *exif)
{
  if(!exif) return;
  GDateTime *gdt = dt_datetime_exif_to_gdatetime(exif, darktable.utc_tz);
  if(gdt)
  {
    img->exif_datetime_taken = g_date_time_difference(gdt, darktable.origin_gdt);
    g_date_time_unref(gdt);
  }
  else img->exif_datetime_taken = 0;
}

gboolean dt_datetime_img_to_exif(char *exif, const size_t exif_size, const dt_image_t *img)
{
  return dt_datetime_gtimespan_to_exif(exif, exif_size, img->exif_datetime_taken);
}

GDateTime *dt_datetime_exif_to_gdatetime(const char *exif, const GTimeZone *tz)
{
  dt_datetime_t dt;
  if(dt_datetime_exif_to_numbers(&dt, exif))
  {
    GDateTime *gdt = g_date_time_new((GTimeZone *)tz, dt.year, dt.month, dt.day,
                                     dt.hour, dt.minute, dt.second);
    if(gdt)
    {
      if(dt.msec)
      {
        GDateTime *gdt2 = g_date_time_add(gdt, dt.msec * 1000);
        g_date_time_unref(gdt);
        return gdt2;
      }
    }
    return gdt;
  }
  return NULL;
}

gboolean dt_datetime_gdatetime_to_exif(char *exif, const size_t exif_size, GDateTime *gdt)
{
  if(!exif || !exif_size || !gdt) return FALSE;
  exif[0] = '\0';
  char *sdt = g_date_time_format(gdt, DT_DATETIME_EXIF_FORMAT);
  if(sdt)
  {
    if(exif_size == DT_DATETIME_LENGTH)
    {
      // the format %f seems not to be available in glib2.0  before version 2.70
      char *sdt2 = _datetime_append_msec(sdt, gdt);
      g_free(sdt);
      sdt = sdt2;
    }
    g_strlcpy(exif, sdt, exif_size);
    g_free(sdt);
    return TRUE;
  }
  return FALSE;
}

GDateTime *dt_datetime_img_to_gdatetime(const dt_image_t *img, const GTimeZone *tz)
{
  // GTimeSpan is UTC based. Therefore we have to cheat a little bit to get image datetime
  if(!tz) return NULL;
  GDateTime *gdt = g_date_time_add(darktable.origin_gdt, img->exif_datetime_taken);
  if(gdt)
  {
    dt_datetime_t dt;
    if(_datetime_gdatetime_to_numbers(&dt, gdt))
    {
      g_date_time_unref(gdt);
      gdt = g_date_time_new((GTimeZone *)tz, dt.year, dt.month, dt.day,
                            dt.hour, dt.minute, (double)dt.second);
      return gdt;
    }
  }
  return NULL;
}

gboolean dt_datetime_entry_to_exif(char *exif, const size_t exif_size, const char *entry)
{
  if(!exif || !exif_size) return FALSE;
  exif[0] = '\0';

  if(strcmp(entry, "now") == 0)
  {
    dt_datetime_now_to_exif(exif);
    return TRUE;
  }

  if(strlen(entry) > DT_DATETIME_LENGTH - 1)
    return FALSE;
  char idt[DT_DATETIME_LENGTH];
  g_strlcpy(idt, DT_DATETIME_ORIGIN, sizeof(idt));
  memcpy(idt, entry, strlen(entry));
  idt[4] = idt[7] = '-';
  GDateTime *gdt = g_date_time_new_from_iso8601(idt, darktable.utc_tz);
  if(gdt)
  {
    const gboolean res = dt_datetime_gdatetime_to_exif(exif, exif_size, gdt);
    g_date_time_unref(gdt);
    return res;
  }
  return FALSE;
}

gboolean dt_datetime_entry_to_exif_upper_bound(char *exif, const size_t exif_size, const char *entry)
{
  if(!exif || !exif_size) return FALSE;
  exif[0] = '\0';

  if(strcmp(entry, "now") == 0)
  {
    dt_datetime_now_to_exif(exif);
    return TRUE;
  }

  const int len = strlen(entry);
  if(len > DT_DATETIME_LENGTH - 1)
    return FALSE;
  char idt[DT_DATETIME_LENGTH];
  g_strlcpy(idt, DT_DATETIME_ORIGIN, sizeof(idt));
  memcpy(idt, entry, strlen(entry));
  idt[4] = idt[7] = '-';
  GDateTime *gdt = g_date_time_new_from_iso8601(idt, darktable.utc_tz);
  if(gdt)
  {
    GDateTime *gdt2 = NULL;
    if(len < 7)
      gdt2 = g_date_time_add_years(gdt, 1);
    else if(len < 10)
      gdt2 = g_date_time_add_months(gdt, 1);
    else if(len < 13)
      gdt2 = g_date_time_add_days(gdt, 1);
    else if(len < 16)
      gdt2 = g_date_time_add_hours(gdt, 1);
    else if(len < 19)
      gdt2 = g_date_time_add_minutes(gdt, 1);
    else if(len < 23)
      gdt2 = g_date_time_add_seconds(gdt, 1);
    else
      gdt2 = g_date_time_add(gdt, 2);
    g_date_time_unref(gdt);
    if(gdt2)
    {
      GDateTime *gdt3 = g_date_time_add(gdt2, -1);
      g_date_time_unref(gdt2);
      gdt = gdt3;
      if(gdt)
      {
        const gboolean res = dt_datetime_gdatetime_to_exif(exif, exif_size, gdt);
        g_date_time_unref(gdt);
        return res;
      }
    }
  }
  return FALSE;
}

void dt_datetime_add_subsec_to_exif(char *exif, const size_t exif_size, const char*subsec)
{
  if(!exif || exif_size < DT_DATETIME_EXIF_LENGTH + 1) return;

  g_strlcpy(&exif[DT_DATETIME_EXIF_LENGTH - 1], ".000000", exif_size - DT_DATETIME_EXIF_LENGTH + 1);
  for(int i = 0; i < 6 && subsec[i] != '\0' && (DT_DATETIME_EXIF_LENGTH + i < exif_size - 1); i++)
    exif[DT_DATETIME_EXIF_LENGTH + i] = subsec[i];
  exif[exif_size - 1] = '\0';
}

gboolean dt_datetime_gtimespan_to_exif(char *sdt, const size_t sdt_size, const GTimeSpan gts)
{
  if(!sdt || !sdt_size) return FALSE;
  sdt[0] = '\0';
  if(!gts) return FALSE;
  GDateTime *gdt = g_date_time_add(darktable.origin_gdt, gts);
  if(gdt)
  {
    const gboolean res = dt_datetime_gdatetime_to_exif(sdt, sdt_size, gdt);
    g_date_time_unref(gdt);
    return res;
  }
  return FALSE;
}

GTimeSpan dt_datetime_exif_to_gtimespan(const char *sdt)
{
  GTimeSpan gts = 0;
  if(!sdt) return gts;
  GDateTime *gdt = dt_datetime_exif_to_gdatetime(sdt, darktable.utc_tz);
  if(gdt)
  {
    gts = g_date_time_difference(gdt, darktable.origin_gdt);
    g_date_time_unref(gdt);
  }
  return gts;
}

gboolean dt_datetime_gtimespan_to_numbers(dt_datetime_t *dt, const GTimeSpan gts)
{
  GDateTime *gdt = g_date_time_add(darktable.origin_gdt, gts);
  if(gdt)
  {
    const gboolean res = _datetime_gdatetime_to_numbers(dt, gdt);
    g_date_time_unref(gdt);
    return res;
  }
  return FALSE;
}

GDateTime *dt_datetime_gtimespan_to_gdatetime(const GTimeSpan gts)
{
  return g_date_time_add(darktable.origin_gdt, gts);
}

GTimeSpan dt_datetime_numbers_to_gtimespan(const dt_datetime_t *dt)
{
  if(!dt) return 0;
  GDateTime *gdt = g_date_time_new(darktable.utc_tz,
                                   dt->year, dt->month, dt->day,
                                   dt->hour, dt->minute, (double)dt->second);
  return _gdatetime_to_gtimespan(gdt);
}

GTimeSpan dt_datetime_gdatetime_to_gtimespan(GDateTime *gdt)
{
  if(gdt)
    return g_date_time_difference(gdt, darktable.origin_gdt);
  else
    return 0;
}

GDateTime *dt_datetime_gdatetime_add_numbers(GDateTime *dte, const dt_datetime_t numbers, const gboolean add)
{
  const int s = add ? 1 : -1;

  GDateTime *dt2 = g_date_time_add_years(dte, s * numbers.year);
  GDateTime *dt = g_date_time_add_months(dt2, s * numbers.month);
  g_date_time_unref(dt2);
  dt2 = g_date_time_add_days(dt, s * numbers.day);
  g_date_time_unref(dt);
  dt = g_date_time_add_hours(dt2, s * numbers.hour);
  g_date_time_unref(dt2);
  dt2 = g_date_time_add_minutes(dt, s * numbers.minute);
  g_date_time_unref(dt);
  dt = g_date_time_add_seconds(dt2, s * numbers.second);
  g_date_time_unref(dt2);
  return dt;
}

GTimeSpan dt_datetime_gtimespan_add_numbers(const GTimeSpan dt, const dt_datetime_t numbers, const gboolean add)
{
  GDateTime *dte = dt_datetime_gtimespan_to_gdatetime(dt);
  GDateTime *dt2 = dt_datetime_gdatetime_add_numbers(dte, numbers, add);
  GTimeSpan ret = dt_datetime_gdatetime_to_gtimespan(dt2);
  g_date_time_unref(dte);
  g_date_time_unref(dt2);
  return ret;
}

gboolean dt_datetime_exif_add_numbers(const gchar *exif, const dt_datetime_t numbers, const gboolean add,
                                      gchar **result)
{
  GDateTime *dte = dt_datetime_exif_to_gdatetime(exif, darktable.utc_tz);
  if(!dte) return FALSE;
  GDateTime *dt2 = dt_datetime_gdatetime_add_numbers(dte, numbers, add);
  char txt[DT_DATETIME_EXIF_LENGTH];
  dt_datetime_gdatetime_to_exif(txt, DT_DATETIME_EXIF_LENGTH, dt2);
  g_date_time_unref(dte);
  g_date_time_unref(dt2);
  *result = g_strdup(txt);
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
