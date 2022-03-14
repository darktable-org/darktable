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

gboolean dt_datetime_exif_to_numbers(dt_datetime_t *dt, const char *exif)
{
  if(exif && *exif && dt)
  {
    char sdt[DT_DATETIME_LENGTH];
    g_strlcpy(sdt, exif, sizeof(sdt));
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

gboolean dt_datetime_img_to_local(char *local, const size_t local_size,
                                  const dt_image_t *img, const gboolean milliseconds)
{
  gboolean res = FALSE;
  GDateTime *gdt = dt_datetime_exif_to_gdatetime(img->exif_datetime_taken, darktable.utc_tz);

  if(gdt)
  {
    char *sdt = g_date_time_format(gdt, milliseconds ? "%a %x %X.%f" : "%a %x %X");
    if(sdt)
    {
      if(milliseconds)
      { // keep only milliseconds
        char *p = g_strrstr(sdt, ".");
        for(int i = 0; i < 4 && *p != '\0'; i++) p++;
        *p = '\0';
      }
      g_strlcpy(local, sdt, local_size);
      g_free(sdt);
      res = TRUE;
    }
    else
      res = FALSE;
    g_date_time_unref(gdt);
  }
  else
  {
    g_strlcpy(local, img->exif_datetime_taken, local_size);
    res = TRUE;
  }
  return res;
}

void dt_datetime_unix_lt_to_exif(char *exif, const size_t exif_len, const time_t *unix)
{
  struct tm tt;
  (void)localtime_r(unix, &tt);
  strftime(exif, exif_len, "%Y:%m:%d %H:%M:%S", &tt);
}

void dt_datetime_unix_lt_to_img(dt_image_t *img, const time_t *unix)
{
  dt_datetime_unix_lt_to_exif(img->exif_datetime_taken, DT_DATETIME_LENGTH, unix);
}

void dt_datetime_now_to_exif(char *exif)
{
  const time_t now = time(NULL);
  dt_datetime_unix_lt_to_exif(exif, DT_DATETIME_EXIF_LENGTH, &now);
}

void dt_datetime_exif_to_img(dt_image_t *img, const char *exif)
{
  g_strlcpy(img->exif_datetime_taken, exif, sizeof(img->exif_datetime_taken));
}

void dt_datetime_img_to_exif(char *exif, const dt_image_t *img)
{
  g_strlcpy(exif, img->exif_datetime_taken, DT_DATETIME_LENGTH);
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

void dt_datetime_gdatetime_to_exif(char *exif, const size_t exif_len, GDateTime *gdt)
{
  char *dt = g_date_time_format(gdt, "%Y:%m:%d %H:%M:%S.%f");
  g_strlcpy(exif, dt, exif_len);
  g_free(dt);
}

GDateTime *dt_datetime_img_to_gdatetime(const dt_image_t *img, const GTimeZone *tz)
{
  return dt_datetime_exif_to_gdatetime(img->exif_datetime_taken, tz);
}

gboolean dt_datetime_img_to_tm_lt(struct tm *tt, const dt_image_t *img)
{
  return _datetime_exif_to_tm(img->exif_datetime_taken, tt);
}

gboolean dt_datetime_img_to_numbers(dt_datetime_t *dt, const dt_image_t *img)
{
  return dt_datetime_exif_to_numbers(dt, img->exif_datetime_taken);
}

void dt_datetime_now_to_numbers(dt_datetime_t *dt)
{
  GDateTime *now = g_date_time_new_now_local();
  _datetime_gdatetime_to_numbers(dt, now);
  g_date_time_unref(now);
}

gboolean dt_datetime_entry_to_exif(char *exif, const size_t exif_len, const char *entry)
{
  if(strlen(entry) > DT_DATETIME_LENGTH - 1)
    return FALSE;
  char idt[DT_DATETIME_LENGTH];
  g_strlcpy(idt, DT_DATETIME_ORIGIN, sizeof(idt));
  memcpy(idt, entry, strlen(entry));
  idt[4] = idt[7] = '-';
  GDateTime *gdt = g_date_time_new_from_iso8601(idt, darktable.utc_tz);
  if(gdt)
  {
    dt_datetime_gdatetime_to_exif(exif, exif_len, gdt);
    g_date_time_unref(gdt);
    return TRUE;
  }
  return FALSE;
}

gboolean dt_datetime_entry_to_exif_upper_bound(char *exif, const size_t exif_len, const char *entry)
{
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
        dt_datetime_gdatetime_to_exif(exif, exif_len, gdt);
        g_date_time_unref(gdt);
        return TRUE;
      }
    }
  }
  return FALSE;
}

void dt_datetime_add_subsec_to_exif(char *exif, const size_t exif_len, const char*subsec)
{
  if(exif_len < DT_DATETIME_EXIF_LENGTH + 1) return;

  g_strlcpy(&exif[DT_DATETIME_EXIF_LENGTH - 1], ".000000", exif_len - DT_DATETIME_EXIF_LENGTH + 1);
  for(int i = 0; i < 6 && subsec[i] != '\0' && (DT_DATETIME_EXIF_LENGTH + i < exif_len - 1); i++)
    exif[DT_DATETIME_EXIF_LENGTH + i] = subsec[i];
  exif[exif_len - 1] = '\0';
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
