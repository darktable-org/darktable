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
#pragma once

#include <glib.h>
#include "common/image.h"

#define DT_DATETIME_ORIGIN "0001-01-01 00:00:00.000"
#define DT_DATETIME_EPOCH "1970-01-01 00:00:00.000"
//  #define DT_DATETIME_LENGTH 24 // defined in image.h
#define DT_DATETIME_EXIF_LENGTH 20

typedef struct dt_datetime_t
{
  gint year;
  gint month;
  gint day;
  gint hour;
  gint minute;
  gint second;
  gint msec;
} dt_datetime_t;

// exif datetime to numbers. Returns TRUE if OK.
gboolean dt_datetime_exif_to_numbers(dt_datetime_t *dt, const char *exif);

// gtimespan to display local string. Returns TRUE if OK.
gboolean dt_datetime_gtimespan_to_local(char *local, const size_t local_size, const GTimeSpan gts);

// img cache datetime to display local string. Returns TRUE if OK.
gboolean dt_datetime_img_to_local(char *local, const size_t local_size,
                                  const dt_image_t *img, const gboolean milliseconds);

// unix datetime to img cache datetime
void dt_datetime_unix_lt_to_img(dt_image_t *img, const time_t *unix);

// unix datetime to exif datetime
void dt_datetime_unix_lt_to_exif(char *exif, const size_t exif_len, const time_t *unix);

// current datetime to exif
void dt_datetime_now_to_exif(char *exif);

// exif datetime to img cache datetime
void dt_datetime_exif_to_img(dt_image_t *img, const char *exif);
// img cache datetime to exif datetime
void dt_datetime_img_to_exif(char *exif, const dt_image_t *img);

// exif datetime to GDateTime. Returns NULL if NOK. Should be freed by g_date_time_unref().
GDateTime *dt_datetime_exif_to_gdatetime(const char *exif, const GTimeZone *tz);
// GDateTime to exif datetime.
void dt_datetime_gdatetime_to_exif(char *exif, const size_t exif_len, GDateTime *gdt);

// img cache datetime to GDateTime. Returns NULL if NOK. Should be freed by g_date_time_unref().
GDateTime *dt_datetime_img_to_gdatetime(const dt_image_t *img, const GTimeZone *tz);

// img cache datetime to unix tm. Returns TRUE if OK.
gboolean dt_datetime_img_to_tm_lt(struct tm *tt, const dt_image_t *img);

// img cache datetime to numbers. Returns TRUE if OK.
gboolean dt_datetime_img_to_numbers(dt_datetime_t *dt, const dt_image_t *img);

// current datetime to numbers. Returns TRUE if OK.
void dt_datetime_now_to_numbers(dt_datetime_t *dt);

// progressive manual entry datetime to exif datetime
gboolean dt_datetime_entry_to_exif(char *exif, const size_t exif_len, const char *entry);
// progressive manual entry datetime to exif datetime bound
gboolean dt_datetime_entry_to_exif_upper_bound(char *exif, const size_t exif_len, const char *entry);

// add subsec (decimal numbers) to exif datetime
void dt_datetime_add_subsec_to_exif(char *exif, const size_t exif_len, const char*subsec);

// get datetime string from gtimespan
char *dt_datetime_gtimespan_to_sdatetime(const GTimeSpan gts, const gboolean msec);
// get gtimespan from datetime string
GTimeSpan dt_datetime_sdatetime_to_gtimespan(const char *sdt);

// get datetime numbers from gtimespan
gboolean dt_datetime_gtimespan_to_numbers(dt_datetime_t *dt, const GTimeSpan gts);
// get gtimespan from datetime numbers
GTimeSpan dt_datetime_numbers_to_gtimespan(const dt_datetime_t *dt);

// get gtimespan from gdatetime
GTimeSpan dt_datetime_gdatetime_to_gtimespan(GDateTime *gdt);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
