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

#define DT_DATETIME_ORIGIN "0001-01-01 00:00:00"
#define DT_DATETIME_EPOCH "1970-01-01 00:00:00"
//  #define DT_DATETIME_LENGTH 24 // defined in image.h
#define DT_DATETIME_EXIF_LENGTH 20

// time_t to display local string. Returns TRUE if OK.
gboolean dt_datetime_unix_lt_to_local(char *local, const size_t local_size, const time_t *unix);

// img cache datetime to display local string. Returns TRUE if OK.
gboolean dt_datetime_img_to_local(char *local, const size_t local_size, const dt_image_t *img);

// unix datetime to img cache datetime
void dt_datetime_unix_lt_to_img(dt_image_t *img, const time_t *unix);

// exif datetime string to unix datetime. Returns TRUE if OK.
gboolean dt_datetime_exif_to_unix_lt(time_t *unix, const char *exif);
// unix datetime to exif datetime
void dt_datetime_unix_lt_to_exif(char *exif, size_t exif_len, const time_t *unix);

// current datetime to exif
void dt_datetime_now_to_exif(char *exif, size_t exif_len);

// exif datetime to img cache datetime
void dt_datetime_exif_to_img(dt_image_t *img, const char *exif);
// img cache datetime to exif datetime
void dt_datetime_img_to_exif(char *exif, const int exif_lgth, const dt_image_t *img);

// exif datetime to GDateTime. Returns NULL if NOK. Should be freed by g_date_time_unref().
GDateTime *dt_datetime_exif_to_gdatetime(const char *exif, const GTimeZone *tz);

// img cache datetime to GDateTime. Returns NULL if NOK. Should be freed by g_date_time_unref().
GDateTime *dt_datetime_img_to_gdatetime(const dt_image_t *img, const GTimeZone *tz);

// exif img cache datetime to unix tm. Returns TRUE if OK.
gboolean dt_datetime_img_to_tm_lt(struct tm *tt, const dt_image_t *img);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
