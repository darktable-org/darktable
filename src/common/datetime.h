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

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define DT_DATETIME_EXIF_LENGTH 20  // exif format string length

// The GTimeSpan saved in db is an offset to datetime_origin (0001:01:01 00:00:00)
// Datetime_taken is to be displayed and stored in XMP without time zone conversion
// The other timestamps consider the timezone (GTimeSpan converted from local to UTC)
// The text format of datetime follows the exif format except when local format

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

// initialize datetime
void dt_datetime_init(void);

// exif datetime to numbers. Returns TRUE if OK.
gboolean dt_datetime_exif_to_numbers(dt_datetime_t *dt, const char *exif);

// gtimespan to display local string. Returns TRUE if OK.
gboolean dt_datetime_gtimespan_to_local(char *local, const size_t local_size,
                                        const GTimeSpan gts, const gboolean msec, const gboolean tz);
// gdatetime to display local string. Returns TRUE if OK.
gboolean dt_datetime_gdatetime_to_local(char *local, const size_t local_size,
                                        GDateTime *gdt, const gboolean msec, const gboolean tz);

// exif datetime to numbers without any check about validity of fields. return TRUE of OK.
gboolean dt_datetime_exif_to_numbers_raw(dt_datetime_t *dt, const char *exif);

// img cache datetime to display local string. Returns TRUE if OK.
gboolean dt_datetime_img_to_local(char *local, const size_t local_size,
                                  const dt_image_t *img, const gboolean msec);

// unix datetime to img cache datetime
gboolean dt_datetime_unix_to_img(dt_image_t *img, const time_t *unix);
// unix datetime to exif datetime
gboolean dt_datetime_unix_to_exif(char *exif, const size_t exif_size, const time_t *unix);

// now to exif
void dt_datetime_now_to_exif(char *exif);
// now to gtimespan
GTimeSpan dt_datetime_now_to_gtimespan(void);

// exif datetime to img cache datetime
void dt_datetime_exif_to_img(dt_image_t *img, const char *exif);
// img cache datetime to exif datetime
gboolean dt_datetime_img_to_exif(char *exif, const size_t exif_size, const dt_image_t *img);

// exif datetime to GDateTime. Returns NULL if NOK. Should be freed by g_date_time_unref().
GDateTime *dt_datetime_exif_to_gdatetime(const char *exif, const GTimeZone *tz);
// GDateTime to exif datetime.
gboolean dt_datetime_gdatetime_to_exif(char *exif, const size_t exif_size, GDateTime *gdt);

// img cache datetime to GDateTime. Returns NULL if NOK. Should be freed by g_date_time_unref().
GDateTime *dt_datetime_img_to_gdatetime(const dt_image_t *img, const GTimeZone *tz);

// progressive manual entry datetime to exif datetime
gboolean dt_datetime_entry_to_exif(char *exif, const size_t exif_size, const char *entry);
// progressive manual entry datetime to exif datetime bound
gboolean dt_datetime_entry_to_exif_upper_bound(char *exif, const size_t exif_size, const char *entry);

// add subsec (decimal numbers) to exif datetime
void dt_datetime_add_subsec_to_exif(char *exif, const size_t exif_size, const char*subsec);

// gtimespan to exif datetime
gboolean dt_datetime_gtimespan_to_exif(char *sdt, const size_t sdt_size, const GTimeSpan gts);
// exif datetime to gtimespan
GTimeSpan dt_datetime_exif_to_gtimespan(const char *sdt);

// gtimepsan to datetime numbers
gboolean dt_datetime_gtimespan_to_numbers(dt_datetime_t *dt, const GTimeSpan gts);
// datetime numbers to gtimespan
GTimeSpan dt_datetime_numbers_to_gtimespan(const dt_datetime_t *dt);

// gdatetime to gtimespan
GTimeSpan dt_datetime_gdatetime_to_gtimespan(GDateTime *gdt);
// gtimespan to gdatetime
GDateTime *dt_datetime_gtimespan_to_gdatetime(const GTimeSpan gts);

// add values (represented by dt_datetime_t) to gdatetime
GDateTime *dt_datetime_gdatetime_add_numbers(GDateTime *dte, const dt_datetime_t numbers, const gboolean add);

// add values (represented by dt_datetime_t) to unix datetime
GTimeSpan dt_datetime_gtimespan_add_numbers(const GTimeSpan dt, const dt_datetime_t numbers, const gboolean add);

// add values (represented by dt_datetime_t) to exif datetime
gboolean dt_datetime_exif_add_numbers(const gchar *exif, const dt_datetime_t numbers, const gboolean add,
                                      gchar **result);

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

