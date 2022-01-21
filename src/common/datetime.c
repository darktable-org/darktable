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


static gboolean _datetime_exif_to_tm(const char *exif,  struct tm *tt)
{
  if(*exif)
  {
    if(sscanf(exif,"%d:%d:%d %d:%d:%d",
      &tt->tm_year, &tt->tm_mon, &tt->tm_mday,
      &tt->tm_hour, &tt->tm_min, &tt->tm_sec) == 6)
    {
      tt->tm_year -= 1900;
      tt->tm_mon--;
      tt->tm_isdst = -1;    // no daylight saving time
      return TRUE;
    }
  }
  return FALSE;
}

gboolean dt_datetime_unix_lt_to_local(char *local, const size_t local_size, const time_t *unix)
{
  struct tm tm_val;
  // just %c is too long and includes a time zone that we don't know from exif
  const size_t datetime_len = strftime(local, local_size, "%a %x %X", localtime_r(unix, &tm_val));
  if(datetime_len > 0)
  {
    const gboolean valid_utf = g_utf8_validate(local, datetime_len, NULL);
    if(valid_utf)
      return TRUE;
    else
    {
      GError *error = NULL;
      gchar *local_datetime = g_locale_to_utf8(local, datetime_len, NULL, NULL, &error);
      if(local_datetime)
      {
        g_strlcpy(local, local_datetime, local_size);
        g_free(local_datetime);
        return TRUE;
      }
      else
      {
        fprintf(stderr, "[metadata timestamp] could not convert '%s' to UTF-8: %s\n", local, error->message);
        g_error_free(error);
        return FALSE;
      }
    }
  }
  else
    return FALSE;
}

gboolean dt_datetime_img_to_local(char *local, const size_t local_size, const dt_image_t *img)
{
  struct tm tt = { 0 };
  if(_datetime_exif_to_tm(img->exif_datetime_taken, &tt))
  {
    const time_t timestamp = mktime(&tt);
    return dt_datetime_unix_lt_to_local(local, local_size, &timestamp);
  }
  else
  {
    g_strlcpy(local, img->exif_datetime_taken, local_size);
    return TRUE;
  }
}

gboolean dt_datetime_exif_to_unix_lt(time_t *unix, const char *exif)
{
  struct tm tt= {0};
  if(_datetime_exif_to_tm(exif, &tt))
  {
    *unix = mktime(&tt);
    return TRUE;
  }
  return FALSE;
}

void dt_datetime_unix_lt_to_exif(char *exif, size_t exif_len, const time_t *unix)
{
  struct tm tt;
  (void)localtime_r(unix, &tt);
  strftime(exif, exif_len, "%Y:%m:%d %H:%M:%S", &tt);
}

void dt_datetime_unix_lt_to_img(dt_image_t *img, const time_t *unix)
{
  dt_datetime_unix_lt_to_exif(img->exif_datetime_taken, DT_DATETIME_LENGTH, unix);
}

void dt_datetime_now_to_exif(char *exif, size_t exif_len)
{
  const time_t now = time(NULL);
  dt_datetime_unix_lt_to_exif(exif, exif_len, &now);
}

void dt_datetime_exif_to_img(dt_image_t *img, const char *exif)
{
  g_strlcpy(img->exif_datetime_taken, exif, sizeof(img->exif_datetime_taken));
}

void dt_datetime_img_to_exif(char *exif, const int exif_lgth, const dt_image_t *img)
{
  g_strlcpy(exif, img->exif_datetime_taken, exif_lgth);
}

GDateTime *dt_datetime_exif_to_gdatetime(const char *exif, const GTimeZone *tz)
{
  gint year;
  gint month;
  gint day;
  gint hour;
  gint minute;
  gint second;
  gint millisecond;

  const int nb = sscanf(exif, "%d:%d:%d %d:%d:%d,%d", (int *)&year, (int *)&month, (int *)&day,
                (int *)&hour, (int *)&minute, (int *)&second, (int *)&millisecond);
  GDateTime *gdt = NULL;
  if(nb >= 6)
  {
    gdt = g_date_time_new((GTimeZone *)tz, year, month, day, hour, minute, second);
    if(nb == 7)
    {
      GDateTime *gdt2 = g_date_time_add(gdt, millisecond * 1000);
      g_date_time_unref(gdt);
      return gdt2;
    }
  }
  return gdt;
}

GDateTime *dt_datetime_img_to_gdatetime(const dt_image_t *img, const GTimeZone *tz)
{
  return dt_datetime_exif_to_gdatetime(img->exif_datetime_taken, tz);
}

gboolean dt_datetime_img_to_tm_lt(struct tm *tt, const dt_image_t *img)
{
  return _datetime_exif_to_tm(img->exif_datetime_taken, tt);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
