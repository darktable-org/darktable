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
#include "common/image.h"

// time_t to display local string
gboolean dt_datetime_unix_to_local(char *local, const size_t local_size, const time_t *unix);

// img cache datetime to display local string
gboolean dt_datetime_img_to_local(const dt_image_t *img, char *local, const size_t local_size);

// unix datetime to img cache datetime
void dt_datetime_unix_to_img(dt_image_t *img, const time_t *unix);

// exif datetime string to unix datetime
gboolean dt_datetime_exif_to_unix(const char *exif_datetime, time_t *unix);
// unix datetime to exif datetime
void dt_datetime_unix_to_exif(char *datetime, size_t datetime_len, const time_t *unix);

// current datetime to exif
void dt_datetime_now_to_exif(char *datetime, size_t datetime_len);

// exif datetime to img cache datetime
void dt_datetime_exif_to_img(dt_image_t *img, const char *datetime);
// exif datetime to img cache datetime
void dt_datetime_img_to_exif(const dt_image_t *img, char *datetime);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
