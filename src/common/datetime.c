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


gboolean dt_datetime_unix_to_local(char *local, const size_t local_size, const time_t *unix)
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

gboolean dt_datetime_img_to_local(const dt_image_t *img, char *local, const size_t local_size)
{
  struct tm tt_exif = { 0 };
  if(sscanf(img->exif_datetime_taken, "%d:%d:%d %d:%d:%d", &tt_exif.tm_year, &tt_exif.tm_mon,
            &tt_exif.tm_mday, &tt_exif.tm_hour, &tt_exif.tm_min, &tt_exif.tm_sec) == 6)
  {
    tt_exif.tm_year -= 1900;
    tt_exif.tm_mon--;
    tt_exif.tm_isdst = -1;
    const time_t exif_timestamp = mktime(&tt_exif);
    return dt_datetime_unix_to_local(local, local_size, &exif_timestamp);
  }
  else
  {
    g_strlcpy(local, img->exif_datetime_taken, local_size);
    return TRUE;
  }
}

gboolean dt_datetime_exif_to_unix(const char *exif_datetime, time_t *unix)
{
  if(*exif_datetime)
  {
    struct tm exif_tm= {0};
    if(sscanf(exif_datetime,"%d:%d:%d %d:%d:%d",
      &exif_tm.tm_year,
      &exif_tm.tm_mon,
      &exif_tm.tm_mday,
      &exif_tm.tm_hour,
      &exif_tm.tm_min,
      &exif_tm.tm_sec) == 6)
    {
      exif_tm.tm_year -= 1900;
      exif_tm.tm_mon--;
      exif_tm.tm_isdst = -1;    // no daylight saving time
      *unix = mktime(&exif_tm);
      return TRUE;
    }
  }
  return FALSE;
}

void dt_datetime_unix_to_exif(char *datetime, size_t datetime_len, const time_t *unix)
{
  struct tm tt;
  (void)localtime_r(unix, &tt);
  strftime(datetime, datetime_len, "%Y:%m:%d %H:%M:%S", &tt);
}

void dt_datetime_unix_to_img(dt_image_t *img, const time_t *unix)
{
  struct tm result;
  strftime(img->exif_datetime_taken, DT_DATETIME_LENGTH, "%Y:%m:%d %H:%M:%S",
           localtime_r(unix, &result));
}

void dt_datetime_now_to_exif(char *datetime, size_t datetime_len)
{
  const time_t now = time(NULL);
  dt_datetime_unix_to_exif(datetime, datetime_len, &now);
}

void dt_datetime_exif_to_img(dt_image_t *img, const char *datetime)
{
  g_strlcpy(img->exif_datetime_taken, datetime, sizeof(img->exif_datetime_taken));
}

void dt_datetime_img_to_exif(const dt_image_t *img, char *datetime)
{
  g_strlcpy(datetime, img->exif_datetime_taken, sizeof(img->exif_datetime_taken));
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
