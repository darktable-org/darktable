/*
    This file is part of darktable,
    copyright (c) 2019 philippe weyland.

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
#include "common/debug.h"
#include "common/tags.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <gdk/gdkkeysyms.h>
#include <math.h>

const char *dt_export_xmp_keys[]
    = { "Xmp.dc.creator", "Xmp.dc.publisher", "Xmp.dc.title", "Xmp.dc.description", "Xmp.dc.rights",
        "Xmp.dc.subject",

        "Xmp.exif.GPSLatitude", "Xmp.exif.GPSLongitude", "Xmp.exif.GPSAltitude",
        "Xmp.exif.DateTimeOriginal",
        "Xmp.exifEX.LensModel",

        "Exif.Image.DateTimeOriginal", "Exif.Image.Make", "Exif.Image.Model", "Exif.Image.Orientation",
        "Exif.Image.Artist", "Exif.Image.Copyright", "Exif.Image.Rating",

        "Exif.GPSInfo.GPSLatitude", "Exif.GPSInfo.GPSLongitude", "Exif.GPSInfo.GPSAltitude",
        "Exif.GPSInfo.GPSLatitudeRef", "Exif.GPSInfo.GPSLongitudeRef", "Exif.GPSInfo.GPSAltitudeRef",
        "Exif.GPSInfo.GPSVersionID",

        "Exif.Photo.DateTimeOriginal", "Exif.Photo.ExposureTime", "Exif.Photo.ShutterSpeedValue",
        "Exif.Photo.FNumber", "Exif.Photo.ApertureValue", "Exif.Photo.ISOSpeedRatings",
        "Exif.Photo.FocalLengthIn35mmFilm", "Exif.Photo.LensModel", "Exif.Photo.Flash",
        "Exif.Photo.WhiteBalance", "Exif.Photo.UserComment", "Exif.Photo.ColorSpace",

        "Xmp.xmp.CreateDate", "Xmp.xmp.CreatorTool", "Xmp.xmp.Identifier", "Xmp.xmp.Label", "Xmp.xmp.ModifyDate",
        "Xmp.xmp.Nickname","Xmp.xmp.Rating",

        "Iptc.Application2.Subject", "Iptc.Application2.Keywords", "Iptc.Application2.LocationName",
        "Iptc.Application2.City", "Iptc.Application2.SubLocation", "Iptc.Application2.ProvinceState",
        "Iptc.Application2.CountryName", "Iptc.Application2.Copyright", "Iptc.Application2.Caption",

        "Xmp.tiff.ImageWidth","Xmp.tiff.ImageLength","Xmp.tiff.Artist", "Xmp.tiff.Copyright"
       };
const guint dt_export_xmp_keys_n = G_N_ELEMENTS(dt_export_xmp_keys);

// TODO replace the following list by a dynamic exiv2 list able to provide info type
// Here are listed only string or XmpText. Can be added as needed.
const char **dt_lib_export_metadata_get_export_keys(guint *dt_export_keys_n)
{
  *dt_export_keys_n = dt_export_xmp_keys_n;
  return dt_export_xmp_keys;
}

GList *dt_lib_export_metadata_get_presets(const char *name, int32_t *flags)
{
  sqlite3_stmt *stmt;
  *flags = 0;
  GList *list = NULL;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT op_params "
    "FROM data.presets "
    "WHERE operation='export_metadata' AND name=?1",
     -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) == SQLITE_ROW)
  {
    char *params = (void *)sqlite3_column_blob(stmt, 0);
    const int32_t params_size = sqlite3_column_bytes(stmt, 0);
    if (params)
    {
      char *params_end = params + params_size;
      *flags = *(const int *)params;
      params += sizeof(int32_t);
      while (params < params_end)
      {
        const char *tagname = params;
        params += strlen(tagname) + 1;
        const char *formula = params;
        params += strlen(formula) + 1;
        const int size = strlen(tagname) + strlen(formula) + 2;
        char *tags = g_malloc(size);
        memcpy(tags, tagname, size);
        list = g_list_append(list, tags);
      }
    }
  }
  sqlite3_finalize(stmt);
  return list;
}

void dt_lib_export_metadata_delete_presets(const char *name)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "DELETE FROM data.presets "
      "WHERE operation='export_metadata' AND name=?1",
       -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

// only difference with dt_lib_presets_add is writeprotect = 0
void dt_lib_export_metadata_presets_add(const char *name, const char *plugin_name, const int32_t version, const void *params,
                        const int32_t params_size)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM data.presets WHERE name=?1 AND operation=?2 AND op_version=?3", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "INSERT INTO data.presets (name, description, operation, op_version, op_params, "
      "blendop_params, blendop_version, enabled, model, maker, lens, "
      "iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max, "
      "focal_length_min, focal_length_max, writeprotect, "
      "autoapply, filter, def, format) VALUES (?1, '', ?2, ?3, ?4, NULL, 0, 1, '%', "
      "'%', '%', 0, 340282346638528859812000000000000000000, 0, 10000000, 0, 100000000, 0, 1000, 0, 0, 0, 0, 0)",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, params, params_size, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

GList *dt_lib_export_metadata_get_presets_list()
{
  GList *result = NULL;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT name "
    "FROM data.presets "
    "WHERE operation='export_metadata'",
     -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char *presets_name = g_strdup((char *)sqlite3_column_text(stmt, 0));
    result = g_list_append(result, presets_name);
  }
  sqlite3_finalize(stmt);

  return result;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
